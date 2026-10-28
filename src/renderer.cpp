#include "renderer.h"
#include "common.h"
#include "menu.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "MinHook.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3d12.h>

// Win32 后端的前置声明 (imgui_impl_win32 内部实现)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Renderer {

// ------------------------------------------------------------------
// hook 状态
// ------------------------------------------------------------------
typedef long(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef long(__stdcall* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn      oPresent      = nullptr;
static ResizeBuffersFn oResizeBuffers = nullptr;

static WNDPROC  oWndProc     = nullptr;
static HWND     g_Hwnd       = nullptr;

// 渲染后端状态
enum class Backend { None, D3D11, D3D12 };
static Backend g_Backend = Backend::None;
static bool     g_WndProcHooked = false;
static bool     g_DeviceLost = false;   // ResizeBuffers 后需要重建
static bool     g_ShuttingDown = false; // 正在卸载: Present 直接放行, 不再初始化/绘制

// SetCursorPos hook: 游玩时 UE 每帧把系统光标拽回窗口中心 (鼠标锁定, 隐藏光标),
// 不拦的话菜单打开后指针也钉死在中心动不了。菜单打开期间吞掉所有改位调用,
// 光标停在用户鼠标实际所在处; 关闭菜单立即恢复游戏原行为
static BOOL(WINAPI* oSetCursorPos)(int, int) = nullptr;
static BOOL WINAPI hkSetCursorPos(int X, int Y)
{
    if (ModMenu::g_MenuVisible && !g_ShuttingDown)
        return TRUE;
    return oSetCursorPos(X, Y);
}

// D3D11 后端
static ID3D11Device*           g_D3D11Device  = nullptr;
static ID3D11DeviceContext*    g_D3D11Context = nullptr;
static ID3D11RenderTargetView* g_D3D11RTV     = nullptr;   // 后缓冲 RTV (flip-model 必须自己绑定)
static IDXGISwapChain*         g_InitSwapChain = nullptr;  // 初始化时使用的交换链 (多交换链检测)

// D3D12 后端
static ID3D12Device*            g_D3D12Device   = nullptr;
static ID3D12CommandQueue*      g_D3D12Queue    = nullptr;
static ID3D12CommandAllocator*  g_D3D12CmdAlloc = nullptr;
static ID3D12GraphicsCommandList* g_D3D12CmdList = nullptr;
static ID3D12DescriptorHeap*    g_RtvHeap       = nullptr;
static ID3D12DescriptorHeap*    g_SrvHeap       = nullptr;
static ID3D12Fence*             g_Fence         = nullptr;
static HANDLE                   g_FenceEvent    = nullptr;
static UINT64                   g_FenceValue    = 0;
static UINT                     g_RtvIncSize    = 0;
static std::vector<ID3D12Resource*> g_BackBuffers;
static UINT                     g_SrvAllocCount = 0;   // SRV 描述符 bump 分配器
static UINT                     g_SrvHeapSize   = 32;

// ------------------------------------------------------------------
// SRV 描述符分配回调 (供 ImGui_ImplDX12 使用)
// ------------------------------------------------------------------
static void SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    (void)info;
    D3D12_CPU_DESCRIPTOR_HANDLE base = g_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gbase = g_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    UINT inc = g_D3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu->ptr = base.ptr + (UINT64)g_SrvAllocCount * inc;
    gpu->ptr = gbase.ptr + (UINT64)g_SrvAllocCount * inc;
    g_SrvAllocCount = (g_SrvAllocCount + 1) % g_SrvHeapSize;
}
static void SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    (void)info; (void)cpu; (void)gpu; // 环形 bump 分配, 无需释放
}

// ------------------------------------------------------------------
// WndProc hook — 线程安全输入转发
// WndProc 在游戏/窗口线程执行, ImGui 上下文属于渲染线程 (Present),
// 官方明确 ImGui 非线程安全: 绝不能在 WndProc 里直接调 ImGui 函数!
// (曾导致返回主菜单时窗口消息暴增, 跨线程竞争写坏 ImGui 内部容器 → 堆损坏崩溃)
// 方案: WndProc 只把消息翻译成事件入队, 渲染线程每帧 FlushQueuedInput 消费
// ------------------------------------------------------------------
#include <mutex>
#include <vector>
#include <windowsx.h>

struct QueuedInput
{
    enum Kind { Key, MousePos, MouseButton, MouseWheel, MouseWheelH, Char, Focus } K;
    int   KeyIdx;    // ImGuiKey 枚举值 / 鼠标键索引 0-4
    bool  Down;
    bool  Focused;
    float X, Y, Wheel;
    unsigned short Ch;
};
static std::mutex               s_InputMx;
static std::vector<QueuedInput> s_InputQueue;

static void QueueInput(const QueuedInput& e)
{
    std::lock_guard<std::mutex> lk(s_InputMx);
    if (s_InputQueue.size() < 512) // 防失控 (如无消费时消息风暴)
        s_InputQueue.push_back(e);
}

// 渲染线程每帧调用: 把队列中的输入事件注入 ImGui
static void FlushQueuedInput()
{
    if (!ImGui::GetCurrentContext())
        return;
    std::vector<QueuedInput> evts;
    {
        std::lock_guard<std::mutex> lk(s_InputMx);
        evts.swap(s_InputQueue);
    }
    if (evts.empty())
        return;
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& e : evts)
    {
        switch (e.K)
        {
        case QueuedInput::Key:         io.AddKeyEvent((ImGuiKey)e.KeyIdx, e.Down); break;
        case QueuedInput::MousePos:    io.AddMousePosEvent(e.X, e.Y); break;
        case QueuedInput::MouseButton: io.AddMouseButtonEvent(e.KeyIdx, e.Down); break;
        case QueuedInput::MouseWheel:  io.AddMouseWheelEvent(0.f, e.Wheel); break; // 垂直滚轮 → wheel_y (下滚为负, 与官方 Win32 后端一致)
        case QueuedInput::MouseWheelH: io.AddMouseWheelEvent(e.Wheel, 0.f); break; // 横向滚轮 → wheel_x (此前两轴写反, 垂直滚轮被 ImGui 当横向滚动, 内容超宽的窗口就会横滚)
        case QueuedInput::Char:        io.AddInputCharacterUTF16(e.Ch); break;
        case QueuedInput::Focus:       io.AddFocusEvent(e.Focused); break;
        }
    }
}

static ImGuiKey VKToImGuiKey(WPARAM vk)
{
    switch (vk)
    {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_CAPITAL: return ImGuiKey_CapsLock;
    case VK_SCROLL: return ImGuiKey_ScrollLock;
    case VK_NUMLOCK: return ImGuiKey_NumLock;
    case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
    case VK_PAUSE: return ImGuiKey_Pause;
    case VK_NUMPAD0: return ImGuiKey_Keypad0;
    case VK_NUMPAD1: return ImGuiKey_Keypad1;
    case VK_NUMPAD2: return ImGuiKey_Keypad2;
    case VK_NUMPAD3: return ImGuiKey_Keypad3;
    case VK_NUMPAD4: return ImGuiKey_Keypad4;
    case VK_NUMPAD5: return ImGuiKey_Keypad5;
    case VK_NUMPAD6: return ImGuiKey_Keypad6;
    case VK_NUMPAD7: return ImGuiKey_Keypad7;
    case VK_NUMPAD8: return ImGuiKey_Keypad8;
    case VK_NUMPAD9: return ImGuiKey_Keypad9;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_LMENU: return ImGuiKey_LeftAlt;
    case VK_LWIN: return ImGuiKey_LeftSuper;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_RMENU: return ImGuiKey_RightAlt;
    case VK_RWIN: return ImGuiKey_RightSuper;
    case VK_APPS: return ImGuiKey_Menu;
    case '0': return ImGuiKey_0;   case '1': return ImGuiKey_1;
    case '2': return ImGuiKey_2;   case '3': return ImGuiKey_3;
    case '4': return ImGuiKey_4;   case '5': return ImGuiKey_5;
    case '6': return ImGuiKey_6;   case '7': return ImGuiKey_7;
    case '8': return ImGuiKey_8;   case '9': return ImGuiKey_9;
    case 'A': return ImGuiKey_A;   case 'B': return ImGuiKey_B;
    case 'C': return ImGuiKey_C;   case 'D': return ImGuiKey_D;
    case 'E': return ImGuiKey_E;   case 'F': return ImGuiKey_F;
    case 'G': return ImGuiKey_G;   case 'H': return ImGuiKey_H;
    case 'I': return ImGuiKey_I;   case 'J': return ImGuiKey_J;
    case 'K': return ImGuiKey_K;   case 'L': return ImGuiKey_L;
    case 'M': return ImGuiKey_M;   case 'N': return ImGuiKey_N;
    case 'O': return ImGuiKey_O;   case 'P': return ImGuiKey_P;
    case 'Q': return ImGuiKey_Q;   case 'R': return ImGuiKey_R;
    case 'S': return ImGuiKey_S;   case 'T': return ImGuiKey_T;
    case 'U': return ImGuiKey_U;   case 'V': return ImGuiKey_V;
    case 'W': return ImGuiKey_W;   case 'X': return ImGuiKey_X;
    case 'Y': return ImGuiKey_Y;   case 'Z': return ImGuiKey_Z;
    case VK_F1: return ImGuiKey_F1;  case VK_F2: return ImGuiKey_F2;
    case VK_F3: return ImGuiKey_F3;  case VK_F4: return ImGuiKey_F4;
    case VK_F5: return ImGuiKey_F5;  case VK_F6: return ImGuiKey_F6;
    case VK_F7: return ImGuiKey_F7;  case VK_F8: return ImGuiKey_F8;
    case VK_F9: return ImGuiKey_F9;  case VK_F10: return ImGuiKey_F10;
    case VK_F11: return ImGuiKey_F11; case VK_F12: return ImGuiKey_F12;
    default: return ImGuiKey_None;
    }
}

static bool IsInputMessage(UINT msg)
{
    switch (msg)
    {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR: case WM_UNICHAR:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_INPUT: case WM_IME_CHAR: case WM_IME_KEYDOWN:
        return true;
    default:
        return false;
    }
}

// 是否成功绘制过至少一帧 (输入拦截保险: 渲染异常时不拦截游戏输入)
static bool g_RenderedOk = false;

static LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_RenderedOk)
    {
        // 翻译为输入事件入队, 由渲染线程消费 — 本线程绝不触碰 ImGui
        switch (msg)
        {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_KEYUP:   case WM_SYSKEYUP:
        {
            ImGuiKey k = VKToImGuiKey(wParam);
            if (k != ImGuiKey_None)
            {
                QueuedInput e{};
                e.K = QueuedInput::Key;
                e.KeyIdx = (int)k;
                e.Down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                QueueInput(e);
            }
            break;
        }
        case WM_CHAR: case WM_SYSCHAR:
            if (wParam >= 32 && wParam != 127) // 过滤控制字符
            {
                QueuedInput e{};
                e.K = QueuedInput::Char;
                e.Ch = (unsigned short)wParam;
                QueueInput(e);
            }
            break;
        case WM_MOUSEMOVE:
        {
            QueuedInput e{};
            e.K = QueuedInput::MousePos;
            e.X = (float)GET_X_LPARAM(lParam);
            e.Y = (float)GET_Y_LPARAM(lParam);
            QueueInput(e);
            break;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        {
            int btn = (msg >= WM_LBUTTONDOWN && msg <= WM_LBUTTONDBLCLK) ? 0
                    : (msg >= WM_RBUTTONDOWN && msg <= WM_RBUTTONDBLCLK) ? 1 : 2;
            QueuedInput e{};
            e.K = QueuedInput::MouseButton;
            e.KeyIdx = btn;
            e.Down = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ? msg == WM_LBUTTONDOWN
                    : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? msg == WM_RBUTTONDOWN
                    : msg == WM_MBUTTONDOWN); // DBLCLK 视为按下
            QueueInput(e);
            break;
        }
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        {
            QueuedInput e{};
            e.K = QueuedInput::MouseButton;
            e.KeyIdx = 3 + (int)GET_XBUTTON_WPARAM(wParam); // 3 或 4
            e.Down = (msg != WM_XBUTTONUP);
            QueueInput(e);
            break;
        }
        case WM_MOUSEWHEEL:
        {
            QueuedInput e{};
            e.K = QueuedInput::MouseWheel;
            e.Wheel = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
            QueueInput(e);
            break;
        }
        case WM_MOUSEHWHEEL:
        {
            QueuedInput e{};
            e.K = QueuedInput::MouseWheelH;
            e.Wheel = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
            QueueInput(e);
            break;
        }
        case WM_SETFOCUS: case WM_KILLFOCUS:
        {
            QueuedInput e{};
            e.K = QueuedInput::Focus;
            e.Focused = (msg == WM_SETFOCUS);
            QueueInput(e);
            break;
        }
        }

        // 菜单打开时拦截游戏输入 (仅在渲染确认正常时)
        if (ModMenu::g_MenuVisible && IsInputMessage(msg))
            return 0L;
    }

    return CallWindowProcW(oWndProc, hwnd, msg, wParam, lParam);
}

// ------------------------------------------------------------------
// 初始化 ImGui (在第一次 Present 时调用)
// ------------------------------------------------------------------
static void ApplyImGuiStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.FrameRounding = 5.0f;
    st.GrabRounding  = 5.0f;
    st.TabRounding   = 6.0f;
    st.ScrollbarRounding = 6.0f;
    st.WindowBorderSize = 1.0f;
    st.FramePadding  = ImVec2(8, 4);
    st.ItemSpacing   = ImVec2(8, 6);
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
}

static bool InitD3D11(IDXGISwapChain* sc)
{
    HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_D3D11Device);
    if (FAILED(hr))
    {
        ModMenu::Log("[Renderer] D3D11 GetDevice 失败 hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    g_D3D11Device->GetImmediateContext(&g_D3D11Context);

    // 创建后缓冲 RTV (flip-model 交换链 Present 时游戏已解除绑定, 必须自己绑定)
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)))
    {
        ModMenu::Log("[Renderer] GetBuffer 失败");
        return false;
    }
    hr = g_D3D11Device->CreateRenderTargetView(backBuffer, nullptr, &g_D3D11RTV);
    backBuffer->Release();
    if (FAILED(hr))
    {
        ModMenu::Log("[Renderer] CreateRenderTargetView 失败 hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    if (!ImGui_ImplDX11_Init(g_D3D11Device, g_D3D11Context))
    {
        ModMenu::Log("[Renderer] ImGui_ImplDX11_Init 失败");
        return false;
    }
    g_Backend = Backend::D3D11;
    g_InitSwapChain = sc;
    ModMenu::Log("[Renderer] D3D11 后端初始化成功 (交换链 %p)", (void*)sc);
    return true;
}

// 释放 D3D12 半初始化的资源 (失败路径防泄漏)
static void ReleaseD3D12Resources()
{
    for (auto* b : g_BackBuffers) if (b) b->Release();
    g_BackBuffers.clear();
    if (g_D3D12CmdList)  { g_D3D12CmdList->Release();  g_D3D12CmdList = nullptr; }
    if (g_D3D12CmdAlloc) { g_D3D12CmdAlloc->Release(); g_D3D12CmdAlloc = nullptr; }
    if (g_RtvHeap)       { g_RtvHeap->Release();       g_RtvHeap = nullptr; }
    if (g_SrvHeap)       { g_SrvHeap->Release();       g_SrvHeap = nullptr; }
    if (g_Fence)         { g_Fence->Release();         g_Fence = nullptr; }
    if (g_FenceEvent)    { CloseHandle(g_FenceEvent);  g_FenceEvent = nullptr; }
    if (g_D3D12Queue)    { g_D3D12Queue->Release();    g_D3D12Queue = nullptr; }
    if (g_D3D12Device)   { g_D3D12Device->Release();   g_D3D12Device = nullptr; }
}

static bool InitD3D12(IDXGISwapChain* sc)
{
    // D3D12 交换链的 "Device" 实际是创建时传入的 CommandQueue
    ID3D12CommandQueue* queue = nullptr;
    HRESULT hr = sc->GetDevice(__uuidof(ID3D12CommandQueue), (void**)&queue);
    if (FAILED(hr))
    {
        ModMenu::Log("[Renderer] 非 D3D12 交换链 (GetDevice(queue) hr=0x%08lX)", (unsigned long)hr);
        return false;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&device)))
    {
        queue->Release();
        return false;
    }

    g_D3D12Queue  = queue;
    g_D3D12Device = device;

    // RTV 堆
    DXGI_SWAP_CHAIN_DESC scd{};
    sc->GetDesc(&scd);
    UINT bufCount = scd.BufferCount > 0 ? scd.BufferCount : 2;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = bufCount;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, __uuidof(ID3D12DescriptorHeap), (void**)&g_RtvHeap)))
    {
        ReleaseD3D12Resources();
        return false;
    }
    g_RtvIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV 堆 (ImGui 字体纹理)
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = g_SrvHeapSize;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, __uuidof(ID3D12DescriptorHeap), (void**)&g_SrvHeap)))
    {
        ReleaseD3D12Resources();
        return false;
    }

    // 命令分配器 + 命令列表
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_D3D12CmdAlloc)))
    {
        ReleaseD3D12Resources();
        return false;
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_D3D12CmdAlloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_D3D12CmdList)))
    {
        ReleaseD3D12Resources();
        return false;
    }
    g_D3D12CmdList->Close();

    // 围栏
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_Fence)))
    {
        ReleaseD3D12Resources();
        return false;
    }
    g_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_FenceValue = 0;

    // 后缓冲 RTV
    g_BackBuffers.resize(bufCount);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < bufCount; i++)
    {
        if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_BackBuffers[i])))
        {
            ReleaseD3D12Resources();
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvBase;
        handle.ptr += (SIZE_T)(i * g_RtvIncSize);
        device->CreateRenderTargetView(g_BackBuffers[i], nullptr, handle);
    }

    ImGui_ImplDX12_InitInfo info{};
    info.Device = g_D3D12Device;
    info.CommandQueue = g_D3D12Queue;
    info.NumFramesInFlight = bufCount;
    info.RTVFormat = scd.BufferDesc.Format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_SrvHeap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn = SrvFree;
    if (!ImGui_ImplDX12_Init(&info))
    {
        ReleaseD3D12Resources();
        ModMenu::Log("[Renderer] ImGui_ImplDX12_Init 失败");
        return false;
    }

    g_Backend = Backend::D3D12;
    ModMenu::Log("[Renderer] D3D12 后端初始化成功");
    return true;
}

static void ShutdownBackend()
{
    if (g_Backend == Backend::D3D11)
    {
        ImGui_ImplDX11_Shutdown();
        if (g_D3D11RTV)     { g_D3D11RTV->Release();      g_D3D11RTV = nullptr; }
        if (g_D3D11Context) { g_D3D11Context->Release(); g_D3D11Context = nullptr; }
        if (g_D3D11Device)  { g_D3D11Device->Release();  g_D3D11Device = nullptr; }
        g_InitSwapChain = nullptr;
    }
    else if (g_Backend == Backend::D3D12)
    {
        ImGui_ImplDX12_Shutdown();
        for (auto* b : g_BackBuffers) if (b) b->Release();
        g_BackBuffers.clear();
        if (g_D3D12CmdList)  { g_D3D12CmdList->Release();  g_D3D12CmdList = nullptr; }
        if (g_D3D12CmdAlloc) { g_D3D12CmdAlloc->Release(); g_D3D12CmdAlloc = nullptr; }
        if (g_RtvHeap)       { g_RtvHeap->Release();       g_RtvHeap = nullptr; }
        if (g_SrvHeap)       { g_SrvHeap->Release();       g_SrvHeap = nullptr; }
        if (g_Fence)         { g_Fence->Release();         g_Fence = nullptr; }
        if (g_FenceEvent)    { CloseHandle(g_FenceEvent);  g_FenceEvent = nullptr; }
        if (g_D3D12Queue)    { g_D3D12Queue->Release();    g_D3D12Queue = nullptr; }
        if (g_D3D12Device)   { g_D3D12Device->Release();   g_D3D12Device = nullptr; }
    }
    g_Backend = Backend::None;
}

static bool InitImGui(IDXGISwapChain* sc, bool Rebuild = false)
{
    DXGI_SWAP_CHAIN_DESC scd{};
    if (FAILED(sc->GetDesc(&scd)))
    {
        ModMenu::Log("[Renderer] GetDesc 失败");
        return false;
    }
    g_Hwnd = scd.OutputWindow;
    if (!g_Hwnd)
    {
        ModMenu::Log("[Renderer] 交换链没有 OutputWindow");
        return false;
    }

    if (!Rebuild)
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr; // 不保存 ini

        // 加载支持中文的系统字体
        bool fontLoaded = false;
        const wchar_t* fonts[] = {
            L"C:\\Windows\\Fonts\\msyh.ttc",      // 微软雅黑
            L"C:\\Windows\\Fonts\\msyh.ttf",
            L"C:\\Windows\\Fonts\\simhei.ttf",    // 黑体
            L"C:\\Windows\\Fonts\\simsun.ttc",    // 宋体
        };
        for (auto f : fonts)
        {
            char pathUtf8[MAX_PATH]{};
            WideCharToMultiByte(CP_UTF8, 0, f, -1, pathUtf8, MAX_PATH, nullptr, nullptr);
            if (GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES)
            {
                ImFontConfig cfg;
                cfg.OversampleH = 2; cfg.OversampleV = 1;
                io.Fonts->AddFontFromFileTTF(pathUtf8, 18.0f, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                fontLoaded = true;
                break;
            }
        }
        if (!fontLoaded)
            io.Fonts->AddFontDefault();

        ApplyImGuiStyle();

        // WndProc hook
        oWndProc = (WNDPROC)SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
        g_WndProcHooked = (oWndProc != nullptr);
        ModMenu::Log("[Renderer] 窗口 %p, WndProc hook = %d", (void*)g_Hwnd, (int)g_WndProcHooked);
    }

    // Win32 后端 (首次与重建都要初始化; 上层在重建前已 Shutdown)
    ImGui_ImplWin32_Init(g_Hwnd);

    // 优先尝试 D3D12, 失败则回退 D3D11
    if (!InitD3D12(sc))
    {
        if (!InitD3D11(sc))
        {
            ModMenu::Log("[Renderer] 初始化后端失败!");
            ImGui_ImplWin32_Shutdown();
            return false;
        }
    }

    ModMenu::g_Initialized = true;
    ModMenu::Log("[Renderer] ImGui 初始化完成 (Rebuild=%d)", (int)Rebuild);
    return true;
}

// ------------------------------------------------------------------
// Present hook
// ------------------------------------------------------------------
static long __stdcall hkPresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags)
{
    // 正在卸载: 直接放行, 禁止重新初始化 (否则卸载后会调用已释放的代码导致崩溃)
    if (g_ShuttingDown)
        return oPresent(sc, SyncInterval, Flags);

    static bool s_FirstPresentLogged = false;
    if (!s_FirstPresentLogged)
    {
        s_FirstPresentLogged = true;
        ModMenu::Log("[Renderer] Present 首次调用, 开始初始化 ImGui");
    }

    if (!ModMenu::g_Initialized)
    {
        if (!InitImGui(sc))
            return oPresent(sc, SyncInterval, Flags);
    }
    else if (g_DeviceLost)
    {
        // ResizeBuffers 之后重建 (不重建 ImGui context 与字体)
        ShutdownBackend();
        ImGui_ImplWin32_Shutdown();
        g_DeviceLost = false;
        InitImGui(sc, true);
    }

    // 帧逻辑 (功能应用 / 菜单 / ESP)
    Menu::Frame();

    // 鼠标指针: 游玩时游戏隐藏并锁定系统光标 (只有暂停菜单才显示)。
    // mod 菜单打开期间改用 ImGui 自绘指针 + 解除光标圈定, 任意时刻都能操作菜单;
    // 关闭菜单后还原, 游戏行为不受影响
    if (ImGui::GetCurrentContext())
    {
        ImGui::GetIO().MouseDrawCursor = ModMenu::g_MenuVisible;
        if (ModMenu::g_MenuVisible)
            ClipCursor(nullptr);
    }

    if (g_Backend == Backend::D3D11)
    {
        // 多交换链检测: 只在初始化时的交换链上绘制
        static bool s_MultiChainWarned = false;
        if (!s_MultiChainWarned && sc != g_InitSwapChain)
        {
            s_MultiChainWarned = true;
            ModMenu::Log("[Renderer] 警告: 检测到第二个交换链 %p (初始化于 %p), 仅在初始交换链绘制",
                         (void*)sc, (void*)g_InitSwapChain);
        }

        ImGui_ImplDX11_NewFrame();
        FlushQueuedInput(); // 消费 WndProc 线程排队的输入事件
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Menu::Render();
        ImGui::Render();

        if (sc == g_InitSwapChain && g_D3D11RTV)
        {
            // flip-model: 游戏调用 Present 前已解除后缓冲绑定, 必须主动绑定再绘制
            ID3D11RenderTargetView* rtv = g_D3D11RTV;
            g_D3D11Context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            // 解除绑定 (flip-model Present 要求后缓冲无绑定)
            ID3D11RenderTargetView* nullRTV = nullptr;
            g_D3D11Context->OMSetRenderTargets(1, &nullRTV, nullptr);

            if (!g_RenderedOk)
            {
                g_RenderedOk = true;
                ModMenu::Log("[Renderer] 首帧绘制完成");
            }
        }
    }
    else if (g_Backend == Backend::D3D12)
    {
        ImGui_ImplDX12_NewFrame();
        FlushQueuedInput(); // 消费 WndProc 线程排队的输入事件
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Menu::Render();
        ImGui::Render();

        // 渲染到当前后缓冲
        IDXGISwapChain3* sc3 = nullptr;
        UINT idx = 0;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)))
        {
            idx = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
        if (idx < g_BackBuffers.size())
        {
            ID3D12GraphicsCommandList* cl = g_D3D12CmdList;
            g_D3D12CmdAlloc->Reset();
            cl->Reset(g_D3D12CmdAlloc, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_BackBuffers[idx];
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &barrier);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)(idx * g_RtvIncSize);
            const FLOAT clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
            cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
            cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cl->SetDescriptorHeaps(1, &g_SrvHeap);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
            g_RenderedOk = true;

            D3D12_RESOURCE_BARRIER barrierBack{};
            barrierBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrierBack.Transition.pResource = g_BackBuffers[idx];
            barrierBack.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrierBack.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            barrierBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &barrierBack);

            cl->Close();
            ID3D12CommandList* lists[] = { cl };
            g_D3D12Queue->ExecuteCommandLists(1, lists);
            g_D3D12Queue->Signal(g_Fence, ++g_FenceValue);
            g_Fence->SetEventOnCompletion(g_FenceValue, g_FenceEvent);
            WaitForSingleObject(g_FenceEvent, 2000);
        }
    }

    return oPresent(sc, SyncInterval, Flags);
}

// ------------------------------------------------------------------
// ResizeBuffers hook
// ------------------------------------------------------------------
static long __stdcall hkResizeBuffers(IDXGISwapChain* sc, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (ModMenu::g_Initialized)
    {
        g_DeviceLost = true;

        // DXGI 要求: ResizeBuffers 前必须释放所有后缓冲引用并解除管线绑定,
        // 否则返回 DXGI_ERROR_INVALID_CALL (UE 会直接 LowLevelFatalError 崩溃)
        if (g_Backend == Backend::D3D11)
        {
            if (g_D3D11Context)
            {
                ID3D11RenderTargetView* nullRTV = nullptr;
                g_D3D11Context->OMSetRenderTargets(1, &nullRTV, nullptr);
            }
            if (g_D3D11RTV)
            {
                g_D3D11RTV->Release();
                g_D3D11RTV = nullptr;
            }
        }
    }
    return oResizeBuffers(sc, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// ------------------------------------------------------------------
// 初始化: dummy 设备拿到 Present 地址
// ------------------------------------------------------------------
bool InitHooks()
{
    // 创建一个隐藏窗口 + dummy D3D11 设备交换链, 拿到 dxgi.dll 中
    // IDXGISwapChain::Present / ResizeBuffers 的地址 (D3D11/D3D12 通用)
    HWND dummyHwnd = CreateWindowExW(0, L"STATIC", L"Dummy", 0, 0, 0, 100, 100, nullptr, nullptr, nullptr, nullptr);
    if (!dummyHwnd)
    {
        ModMenu::Log("[Renderer] 创建 dummy 窗口失败");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device* dummyDevice = nullptr;
    IDXGISwapChain* dummySwap = nullptr;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &dummySwap, &dummyDevice, nullptr, nullptr);

    if (!dummySwap)
    {
        // 软件渲染回退
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &scd, &dummySwap, &dummyDevice, nullptr, nullptr);
    }

    if (!dummySwap)
    {
        ModMenu::Log("[Renderer] 创建 dummy 交换链失败");
        DestroyWindow(dummyHwnd);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(dummySwap);
    void* presentAddr = vtable[8];       // IDXGISwapChain::Present
    void* resizeAddr = vtable[13];       // IDXGISwapChain::ResizeBuffers

    dummySwap->Release();
    if (dummyDevice) dummyDevice->Release();
    DestroyWindow(dummyHwnd);

    if (MH_Initialize() != MH_OK)
    {
        ModMenu::Log("[Renderer] MH_Initialize 失败");
        return false;
    }

    // SetCursorPos hook (user32 导出函数, 非虚表指针)
    void* setCursorPosAddr = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos");
    if (!setCursorPosAddr
        || MH_CreateHook(setCursorPosAddr, reinterpret_cast<void*>(&hkSetCursorPos),
                         reinterpret_cast<void**>(&oSetCursorPos)) != MH_OK)
    {
        ModMenu::Log("[Renderer] hook SetCursorPos 失败 (游玩时菜单指针可能被拽回中心)");
    }

    if (MH_CreateHook(presentAddr, reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&oPresent)) != MH_OK)
    {
        ModMenu::Log("[Renderer] hook Present 失败");
        return false;
    }
    if (MH_CreateHook(resizeAddr, reinterpret_cast<void*>(&hkResizeBuffers), reinterpret_cast<void**>(&oResizeBuffers)) != MH_OK)
    {
        ModMenu::Log("[Renderer] hook ResizeBuffers 失败");
    }
    MH_EnableHook(MH_ALL_HOOKS);

    ModMenu::Log("[Renderer] Hook 初始化完成 (Present @ %p)", presentAddr);
    return true;
}

void Shutdown()
{
    if (g_ShuttingDown)
        return; // 防止 DLL_PROCESS_DETACH 与 END 键卸载线程重复执行
    g_ShuttingDown = true;

    ModMenu::Log("[Renderer] 开始关闭...");

    // 1. 恢复 WndProc (越早越好, 之后游戏输入恢复正常)
    if (g_WndProcHooked && g_Hwnd && oWndProc)
    {
        SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        g_WndProcHooked = false;
    }

    // 2. 停止绘制并等待渲染线程离开 hook 代码 (Present ~16ms 一帧, 等 3 帧足够)
    ModMenu::g_Initialized = false;
    Sleep(100);

    // 3. 摘除 MinHook (渲染线程已离开 hook 区域)
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Sleep(50);

    // 4. 释放 ImGui 与后端资源
    if (ImGui::GetCurrentContext())
    {
        ShutdownBackend();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    ModMenu::Log("[Renderer] 关闭完成");
}

} // namespace Renderer
