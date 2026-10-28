#pragma once
// EvilNunModMenu 公共头文件

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

namespace ModMenu {

// 简单文件日志
void Log(const char* fmt, ...);
void LogW(const wchar_t* fmt, ...);

// 构建版本戳 (每次构建由 CMake 重新生成, 保证不陈旧)
const char* GetBuildStamp();

// 菜单可见性
extern bool g_MenuVisible;
extern bool g_Initialized;

// 失明开启时吞掉 Philippa 的 ReceiveTick (dllmain ProcessEvent hook 读取):
// 检测逻辑全在 Tick 字节码内 (字段/委托/EntryPoint 均无法拦截), 停掉 BP Tick =
// 永远发现不了玩家; 她的 AI 行为树在 controller 侧独立运行, 巡逻不受影响
extern bool g_BlockPhilippaTick;

// 上帝模式最近一次拦截到"修女处决链入口"的时刻 (dllmain 写; 0 = 无)。
// 处决链被拦 = 拉出已经发生 — 据此在 2 秒内进入残局窗口: 清残留的假藏身状态位
// 并复位修女追捕状态 (2026-09-30 实测确认)。
extern unsigned long long g_CatchBlockedTick;

// 处决链残局: 待修女复位标志 (dllmain 游戏线程立旗, menu.cpp 渲染线程消费)。
// 修女列表 GetNuns() 返回静态 vector 引用且由渲染线程重建 — 游戏线程禁止迭代,
// 否则迭代到释放中的缓冲区直接崩溃 (0x000007ffd... 读 0x8 实测)。
extern bool g_CatchNunResetPending;

// (2026-10-01) 上帝模式只保留处决链拦截 + 残局清理, 藏身保护已按用户要求移除。
// (G 键丢弃功能已移除 — 游戏丢弃链无法可靠重放)

} // namespace ModMenu
