// 菜单界面截图工具 (开发用): 用与 renderer.cpp 完全相同的 ImGui 风格/字体,
// 复刻 menu.cpp 的界面布局与文案 (L() 双语), 逐页截图输出 BMP。
// 编译两次 (CN/EN) 分别生成 README 截图。不参与 DLL 构建。
// 用法: preview_shots.exe <输出目录>
#include "../../src/lang.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <windowsx.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND h, UINT m, WPARAM w, LPARAM l);

static ID3D11Device*           g_Device = nullptr;
static ID3D11DeviceContext*    g_Context = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static IDXGISwapChain*         g_Swap = nullptr;
static bool                    g_Resized = true;
static int                     g_Width = 860, g_Height = 600;
static HWND                    g_Hwnd = nullptr;

static LRESULT WINAPI WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l))
        return 1;
    switch (msg)
    {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g_Width = LOWORD(l); g_Height = HIWORD(l); g_Resized = true; }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void CreateRTV()
{
    if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_Swap->GetBuffer(0, IID_PPV_ARGS(&back))))
    {
        g_Device->CreateRenderTargetView(back, nullptr, &g_RTV);
        back->Release();
    }
}

static bool SaveBMP(const char* path, int w, int h)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_Swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_Device->CreateTexture2D(&desc, nullptr, &st))) { back->Release(); return false; }
    g_Context->CopyResource(st, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(g_Context->Map(st, 0, D3D11_MAP_READ, 0, &mp))) { st->Release(); return false; }

    FILE* f = fopen(path, "wb");
    if (!f) { g_Context->Unmap(st, 0); st->Release(); return false; }
    const int rowSize = ((w * 3 + 3) / 4) * 4;
    unsigned char hdr[54] = { 0 };
    hdr[0] = 'B'; hdr[1] = 'M';
    auto wr = [&](int off, int v) { memcpy(hdr + off, &v, 4); };
    wr(2, 54 + rowSize * h); wr(10, 54); wr(14, 40);
    wr(18, w); wr(22, h); wr(26, 1); wr(28, 24); wr(34, rowSize * h);
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row(rowSize);
    for (int y = h - 1; y >= 0; --y)
    {
        const unsigned char* s = (const unsigned char*)mp.pData + (size_t)y * mp.RowPitch;
        for (int x = 0; x < w; ++x)
        {
            row[x * 3 + 0] = s[x * 4 + 2];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4 + 0];
        }
        fwrite(row.data(), 1, rowSize, f);
    }
    fclose(f);
    g_Context->Unmap(st, 0);
    st->Release();
    return true;
}

// ---------------- 模拟数据 (与 menu.cpp 控件一一对应) ----------------
static bool b_god = true, b_stam = false, b_speed = true, b_jump = true, b_noclip = false;
static bool b_freeze = false, b_blind = true, b_deaf = false, b_staydead = false, b_scale = false;
static bool b_espNun = true, b_espItem = true, b_espGrab = true, b_espPhys = false, b_espDist = true;
static bool b_fov = true, b_gamma = false;
static float f_walk = 400, f_run = 900, f_crouch = 150, f_jump = 800, f_noclip = 1500;
static float f_dead = 30, f_scale = 1.0f, f_fov = 110, f_gamma = 1.0f, f_spawn = 2.0f;
static int   i_tp = 12;
static char  s_espFilter[64] = "";
static char  s_itemFilter[128] = "";
static char  s_tpFilter[64] = "";

static void TabPlayer(int sub)
{
    if (sub == 0)
    {
        ImGui::Checkbox(L("上帝模式", "God Mode"), &b_god);
        ImGui::Checkbox(L("无限体力", "Infinite Stamina"), &b_stam);
        ImGui::Separator();
        ImGui::Button(L("恢复生命 (重新开始受伤逻辑)", "Restore Health (re-enable damage)"));
    }
    else if (sub == 1)
    {
        ImGui::Checkbox(L("启用速度修改", "Enable Speed Override"), &b_speed);
        ImGui::SliderFloat(L("走路速度", "Walk Speed"), &f_walk, 50.f, 2000.f);
        ImGui::SliderFloat(L("跑步速度", "Run Speed"), &f_run, 100.f, 3000.f);
        ImGui::SliderFloat(L("下蹲速度", "Crouch Speed"), &f_crouch, 50.f, 1000.f);
        ImGui::Separator();
        ImGui::Checkbox(L("解锁跳跃", "Unlock Jump"), &b_jump);
        ImGui::SliderFloat(L("跳跃高度", "Jump Height"), &f_jump, 100.f, 5000.f);
        ImGui::Separator();
        ImGui::Checkbox(L("穿墙飞行 (F4)", "Noclip Fly (F4)"), &b_noclip);
        ImGui::SliderFloat(L("飞行速度", "Fly Speed"), &f_noclip, 200.f, 5000.f, "%.0f");
    }
    else
    {
        ImGui::Button(L("获得面具", "Get Mask"));
        ImGui::SliderInt(L("传送点数", "Teleport Charges"), &i_tp, 0, 99);
        ImGui::Button(L("+10 传送点", "+10 Charges"));
    }
}

static void TabNun(int sub)
{
    if (sub == 0)
    {
        ImGui::Checkbox(L("冻结修女", "Freeze Nuns"), &b_freeze);
        ImGui::Checkbox(L("修女失明", "Nuns Blinded"), &b_blind);
        ImGui::Checkbox(L("修女失聪", "Nuns Deafened"), &b_deaf);
        ImGui::Separator();
        ImGui::SliderFloat(L("死亡持续时间 (秒)", "Death Duration (s)"), &f_dead, 5.f, 30.f, L("%.0f 秒", "%.0f s"));
        ImGui::Checkbox(L("永久死亡", "Permanent Death"), &b_staydead);
        ImGui::Button(L("杀死所有修女", "Kill All Nuns"));
        ImGui::SameLine();
        ImGui::Button(L("复活所有修女", "Revive All Nuns"));
        ImGui::Separator();
        ImGui::Button(L("所有修女传送到我面前", "Teleport All Nuns To Me"));
        ImGui::Separator();
        ImGui::Checkbox(L("启用体型修改", "Enable Scale Override"), &b_scale);
        ImGui::SliderFloat(L("修女体型倍率", "Nun Scale Factor"), &f_scale, 0.1f, 3.0f, "%.2fx");
    }
    else
    {
        static double d1 = 420, d2 = 1200, d3 = 900;
        static const double dmin = 0, dmaxSpeed = 2000, dmaxDist = 10000;
        ImGui::Text(L("修女 #1 [Madeline]", "Nun #1 [Madeline]"));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), L("[追击中]", "[CHASING]"));
        ImGui::SliderScalar(L("最大速度", "Max Speed"), ImGuiDataType_Double, &d1, &dmin, &dmaxSpeed);
        ImGui::SliderScalar(L("视野距离", "Sight Range"), ImGuiDataType_Double, &d2, &dmin, &dmaxDist);
        ImGui::SliderScalar(L("听觉距离", "Hearing Range"), ImGuiDataType_Double, &d3, &dmin, &dmaxDist);
        ImGui::Button(L("杀死", "Kill")); ImGui::SameLine();
        ImGui::Button(L("复活", "Revive")); ImGui::SameLine();
        ImGui::Button(L("传到我面前", "Teleport To Me")); ImGui::SameLine();
        ImGui::Button(L("我传过去", "I Go To Her"));
        ImGui::Separator();
        ImGui::Text(L("修女 #2 [Philippa]", "Nun #2 [Philippa]"));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), L("[平静]", "[CALM]"));
    }
}

static void TabESP()
{
    ImGui::Checkbox(L("修女方框 ESP", "Nun Box ESP"), &b_espNun);
    ImGui::Checkbox(L("道具位置高亮", "Item Highlight"), &b_espItem);
    if (b_espItem)
    {
        ImGui::InputTextWithHint("##espfilter", L("过滤道具名 (留空=全部)...", "Filter item names (empty = all)..."),
                                 s_espFilter, sizeof(s_espFilter));
        ImGui::Checkbox(L("任务道具", "Quest Items"), &b_espGrab);
        ImGui::SameLine();
        ImGui::Checkbox(L("物理杂物", "Physics Props"), &b_espPhys);
    }
    ImGui::Checkbox(L("显示距离", "Show Distance"), &b_espDist);
}

static void TabVisual()
{
    ImGui::Checkbox(L("启用 FOV 修改", "Enable FOV Override"), &b_fov);
    ImGui::SliderFloat("FOV", &f_fov, 30.f, 150.f);
    ImGui::Separator();
    ImGui::Checkbox(L("启用 Gamma 修改", "Enable Gamma Override"), &b_gamma);
    ImGui::SliderFloat("Gamma", &f_gamma, 0.5f, 4.f);
    ImGui::Separator();
    ImGui::Button(L("启用控制台作弊 (EnableCheats)", "Enable Console Cheats (EnableCheats)"));
}

static void TabTeleport()
{
    ImGui::TextUnformatted(L("房间 / 场景 / 出生点", "Rooms / Areas / Spawn Points"));
    ImGui::Separator();
    ImGui::InputTextWithHint("##tpfilter", L("搜索传送点...", "Search teleport points..."), s_tpFilter, sizeof(s_tpFilter));
    ImGui::SameLine();
    ImGui::SmallButton("x");
    if (ImGui::BeginChild("##tp", ImVec2(0, 260), true))
    {
        static const char* rooms_cn[] = { "教堂 · 中殿", "修女楼 · 一层走廊", "修女楼 · 二层", "洗衣房",
                                          "厨房", "餐厅", "美术教室", "体操房", "粉房 · 卧室", "地窖入口" };
        static const char* rooms_en[] = { "Church - Nave", "Convent - Floor 1 Hall", "Convent - Floor 2", "Laundry",
                                          "Kitchen", "Dining Hall", "Art Classroom", "Gym", "Pink Room - Bedroom", "Cellar Entrance" };
        const char** rooms = MODMENU_LANG_CN ? rooms_cn : rooms_en;
        for (int i = 0; i < 10; ++i)
        {
            ImGui::PushID(i);
            ImGui::Text("%s", rooms[i]);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
            ImGui::Text(L("%.0f 米", "%.0f m"), 35.0 + i * 23.7);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
            ImGui::SmallButton(L("传送", "Go"));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

static void TabItems()
{
    ImGui::SliderFloat(L("生成距离 (米)", "Spawn Distance (m)"), &f_spawn, 0.5f, 10.f);
    ImGui::InputTextWithHint("##filter", L("搜索物品...", "Search items..."), s_itemFilter, sizeof(s_itemFilter));
    ImGui::SameLine();
    ImGui::Checkbox(L("任务道具", "Quest Items"), &b_espGrab);
    ImGui::SameLine();
    ImGui::Checkbox(L("物理杂物", "Physics Props"), &b_espPhys);
    ImGui::SameLine();
    ImGui::Button(L("重新扫描", "Rescan"));
    ImGui::SameLine();
    ImGui::Button(L("补全列表 (加载全部子关卡)", "Complete List (load all sublevels)"));
    ImGui::SameLine();
    ImGui::Button(L("导出实例清单", "Dump Instance List"));
    ImGui::Text(L("已加载 128 种物品 | 本次注入已生成 3 个",
                  "Loaded 128 item types | 3 spawned this session"));
    ImGui::Separator();
    if (ImGui::BeginChild("物品列表", ImVec2(0, 0), ImGuiChildFlags_Borders))
    {
        ImGui::SeparatorText(L("任务道具", "Quest Items"));
        static const char* items_cn[][2] = {
            {"MainDoorKey 大门钥匙", "MainDoorKey"}, {"Fuse 保险丝", "Fuse"}, {"Grail 圣杯", "Grail"},
            {"Dynamite 炸药", "Dynamite"}, {"Torch 火把", "Torch"}, {"Crowbar 撬棍", "Crowbar"},
            {"Battery 电池", "Battery"}, {"Broom 扫帚", "Broom"}, {"Shovel 铁锹", "Shovel"},
        };
        const char* (*items)[2] = items_cn;
        for (int i = 0; i < 9; ++i)
        {
            ImGui::PushID(i);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", MODMENU_LANG_CN ? items[i][0] : items[i][1]);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
            ImGui::Button(L("生成", "Spawn"));
            ImGui::PopID();
        }
        ImGui::SeparatorText(L("物理杂物", "Physics Props"));
        static const char* phys_cn[][2] = {
            {"Plate 盘子", "Plate"}, {"Bottle 瓶子", "Bottle"}, {"Book 书", "Book"}, {"Cup 杯子", "Cup"},
        };
        for (int i = 0; i < 4; ++i)
        {
            ImGui::PushID(100 + i);
            ImGui::Text("%s", MODMENU_LANG_CN ? phys_cn[i][0] : phys_cn[i][1]);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
            ImGui::Button(L("生成", "Spawn"));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

static void TabObject()
{
    ImGui::Button(L("选取视线中心目标 (F2)", "Pick Object At Crosshair (F2)"));
    ImGui::Separator();
    ImGui::Text(L("名称: Fuse_2", "Name: Fuse_2"));
    ImGui::Text(L("类: NunGrabObject_C", "Class: NunGrabObject_C"));
    ImGui::Text(L("位置: (-120, 340, 88)  距离: 12m", "Loc: (-120, 340, 88)  Dist: 12m"));
    ImGui::Button(L("传送到它旁边", "Teleport To It")); ImGui::SameLine();
    ImGui::Button(L("拉到我面前", "Pull To Me")); ImGui::SameLine();
    ImGui::Button(L("删除", "Delete")); ImGui::SameLine();
    ImGui::Button(L("取消选中", "Clear Selection"));
}

static void TabWorld()
{
    ImGui::Text(L("游戏版本: 1.0.2.0", "Game version: 1.0.2.0"));
    ImGui::Text(L("关卡索引: 7", "Level index: 7"));
    ImGui::Text(L("剩余天数: 3 / 12", "Days remaining: 3 / 12"));
    ImGui::Text(L("玩家坐标: (-1234.5, 678.2, 432.0)", "Player loc: (-1234.5, 678.2, 432.0)"));
    ImGui::Text(L("安全区: 否 | 洗衣房: 是 | 掩体: 否", "Safe zone: No | Laundry: Yes | Bunker: No"));
    ImGui::Text(L("玩家控制器: 有效", "Player controller: valid"));
    ImGui::Separator();
    ImGui::Button(L("完成当前任务", "Complete Current Mission"));
}

static void TabAbout()
{
    ImGui::TextUnformatted("Evil Nun: The Broken Mask Mod Menu");
    ImGui::TextUnformatted("ImGui + MinHook + Dumper-7 SDK");
    ImGui::Text(L("菜单语言 / Menu language: %s", "Menu language / 菜单语言: %s"), MODMENU_LANG_CODE);
    ImGui::Separator();
    ImGui::TextUnformatted(L("F1 - 切换菜单", "F1 - Toggle menu"));
    ImGui::TextUnformatted(L("ESP 在菜单关闭时依然显示", "ESP stays visible while the menu is closed"));
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
    std::string outDir;
    if (cmdLine && *cmdLine)
    {
        while (*cmdLine == L'"') cmdLine++;
        std::wstring w(cmdLine);
        while (!w.empty() && (w.back() == L'"' || w.back() == L'\\')) w.pop_back();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        outDir.resize(n > 1 ? n - 1 : 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, outDir.data(), n, nullptr, nullptr);
    }
    if (outDir.empty()) outDir = "shots";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ShotWin";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    RECT rc{ 0, 0, g_Width, g_Height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_Hwnd = CreateWindowExW(0, wc.lpszClassName, L"menu", WS_OVERLAPPEDWINDOW,
                             40, 40, rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_Hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_Hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &g_Swap, &g_Device, nullptr, &g_Context);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &scd, &g_Swap, &g_Device, nullptr, &g_Context);
    if (FAILED(hr)) return 1;
    CreateRTV();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // 与 renderer.cpp 完全一致的风格
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

    // 与 renderer.cpp 一致的字体
    const wchar_t* fonts[] = { L"C:\\Windows\\Fonts\\msyh.ttc", L"C:\\Windows\\Fonts\\simhei.ttf" };
    bool fontOk = false;
    for (auto fp : fonts)
    {
        if (GetFileAttributesW(fp) == INVALID_FILE_ATTRIBUTES) continue;
        char ansi[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, fp, -1, ansi, MAX_PATH, nullptr, nullptr);
        ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF(ansi, 18.0f, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        fontOk = true;
        break;
    }
    if (!fontOk) io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(g_Hwnd);
    ImGui_ImplDX11_Init(g_Device, g_Context);

    struct Shot { const char* Name; int Tab; int Sub; };
    const Shot shots[] = {
        { "player",   0, 0 }, { "player_move", 0, 1 }, { "player_items", 0, 2 },
        { "items",    1, 0 },
        { "nun",      2, 0 }, { "nun_info", 2, 1 },
        { "teleport", 3, 0 }, { "esp", 4, 0 }, { "visual", 5, 0 },
        { "object",   6, 0 }, { "world", 7, 0 }, { "about", 8, 0 },
    };

    for (const Shot& sh : shots)
    {
        for (int frame = 0; frame < 4; ++frame)
        {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&m); DispatchMessageW(&m); }
            if (g_Resized && g_Width > 0 && g_Height > 0)
            {
                g_Context->OMSetRenderTargets(0, nullptr, nullptr);
                g_Swap->ResizeBuffers(0, g_Width, g_Height, DXGI_FORMAT_UNKNOWN, 0);
                CreateRTV();
                g_Resized = false;
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // 左上角水印 (与游戏内一致)
            const char* wm = L("EvilNunModMenu 已注入 [F1 打开菜单]", "EvilNunModMenu injected [F1 for menu]");
            ImVec2 wsz = ImGui::CalcTextSize(wm);
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(8, 8), ImVec2(8 + wsz.x + 8, 8 + wsz.y + 4),
                                                          IM_COL32(0, 0, 0, 140), 4.0f);
            ImGui::GetBackgroundDrawList()->AddText(ImVec2(12, 10), IM_COL32(120, 220, 120, 255), wm);

            ImGui::SetNextWindowPos(ImVec2(60, 40), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_Always);
            ImGui::Begin(L("Evil Nun ModMenu [F1 切换]", "Evil Nun ModMenu [F1 toggle]"), nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
            if (ImGui::BeginTabBar("#主页"))
            {
                if (ImGui::BeginTabItem(L("玩家", "Player"), nullptr,
                        sh.Tab == 0 ? ImGuiTabItemFlags_SetSelected : 0))
                {
                    if (ImGui::BeginTabBar("玩家子页"))
                    {
                        const int nsub = 3;
                        static const char* subs_cn[] = { "状态", "移动", "道具" };
                        static const char* subs_en[] = { "Status", "Movement", "Items" };
                        const char** subs = MODMENU_LANG_CN ? subs_cn : subs_en;
                        for (int s = 0; s < nsub; ++s)
                            if (ImGui::BeginTabItem(subs[s], nullptr,
                                    (sh.Tab == 0 && sh.Sub == s) ? ImGuiTabItemFlags_SetSelected : 0))
                            { if (sh.Sub == s) TabPlayer(s); ImGui::EndTabItem(); }
                        ImGui::EndTabBar();
                    }
                    ImGui::EndTabItem();
                }
                auto MainTab = [&](const char* label, int idx, void(*fn)()) {
                    if (ImGui::BeginTabItem(label, nullptr, sh.Tab == idx ? ImGuiTabItemFlags_SetSelected : 0))
                    { if (sh.Tab == idx) fn(); ImGui::EndTabItem(); }
                };
                MainTab(L("道具", "Items"), 1, TabItems);
                if (ImGui::BeginTabItem(L("修女AI", "Nun AI"), nullptr,
                        sh.Tab == 2 ? ImGuiTabItemFlags_SetSelected : 0))
                {
                    if (ImGui::BeginTabBar("修女子页"))
                    {
                        static const char* subs_cn[] = { "控制", "信息" };
                        static const char* subs_en[] = { "Control", "Info" };
                        const char** subs = MODMENU_LANG_CN ? subs_cn : subs_en;
                        for (int s = 0; s < 2; ++s)
                            if (ImGui::BeginTabItem(subs[s], nullptr,
                                    (sh.Tab == 2 && sh.Sub == s) ? ImGuiTabItemFlags_SetSelected : 0))
                            { if (sh.Sub == s) TabNun(s); ImGui::EndTabItem(); }
                        ImGui::EndTabBar();
                    }
                    ImGui::EndTabItem();
                }
                MainTab(L("传送", "Teleport"), 3, TabTeleport);
                MainTab("ESP", 4, TabESP);
                MainTab(L("视觉", "Visual"), 5, TabVisual);
                MainTab(L("对象", "Object"), 6, TabObject);
                MainTab(L("世界", "World"), 7, TabWorld);
                MainTab(L("关于", "About"), 8, TabAbout);
                ImGui::EndTabBar();
            }
            ImGui::End();

            ImGui::Render();
            const float clear[4] = { 0.045f, 0.05f, 0.06f, 1.f };
            g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
            g_Context->ClearRenderTargetView(g_RTV, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_Swap->Present(0, 0);
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%s.bmp", outDir.c_str(), MODMENU_LANG_CODE, sh.Name);
        printf("%s: %s\n", SaveBMP(path, g_Width, g_Height) ? "saved" : "FAILED", path);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
