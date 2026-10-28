#pragma once
// ModMenu 主菜单: 帧逻辑 + 渲染

namespace Menu {

// 每帧调用 (在 Present hook 中), 应用各功能 + 处理按键 + 更新上下文
void Frame();

// ImGui 渲染菜单 UI
void Render();

// 上帝模式开关状态 (dllmain 的 ProcessEvent hook 用它拦截伤害事件)
bool IsGodMode();

// 穿墙飞行开关状态 (dllmain 的 ProcessEvent hook 用它禁用飞行期间的丢弃)
bool IsNoclip();

} // namespace Menu
