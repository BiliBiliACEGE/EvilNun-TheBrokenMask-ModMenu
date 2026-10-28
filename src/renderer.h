#pragma once
// DXGI Present Hook + D3D11/D3D12 统一 ImGui 渲染后端

#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3d12.h>

namespace Renderer {

// 初始化 MinHook 并 hook IDXGISwapChain::Present / ResizeBuffers
// (dxgi.dll 的 Present 对 D3D11 与 D3D12 交换链是同一个函数, 一份 hook 通吃)
bool InitHooks();
void Shutdown();

} // namespace Renderer
