#pragma once
// 菜单多语言支持 (编译期切换, 打包两份 DLL: CN 版 / EN 版)
//
// 用法:
//   - 构建时加 -DMODMENU_LANG_EN → 英文菜单; 不加 → 中文菜单 (默认)
//   - 界面字符串一律写 L("中文", "English"), 由编译期选择其一
//   - 物品分组 (ItemClassInfo::Group) 的内部键固定为中文 (定义在本文件的
//     kGroupTask/kGroupPhys), 与语言无关; 显示时经 GroupDisplay() 翻译。
//     因此 CN/EN 两份 DLL 对同一存档/日志/过滤逻辑完全兼容。
//
// 注意: EN 版仍加载 CJK 系统字体 (游戏内物品显示名可能是中文文本), 无副作用。

#include <string>
#include <string_view>

#if defined(MODMENU_LANG_EN)
    #define MODMENU_LANG_CN 0
    #define MODMENU_LANG_CODE "EN"
#else
    #define MODMENU_LANG_CN 1
    #define MODMENU_LANG_CODE "CN"
#endif

// 界面文本: 编译期二选一
#define L(cn, en) (MODMENU_LANG_CN ? (cn) : (en))

namespace Lang {

// 物品分组内部键 (跨语言稳定, 勿改)
inline constexpr const char* kGroupTask = "任务道具";
inline constexpr const char* kGroupPhys = "物理杂物";

// 分组显示名 (UI 用)
inline const char* GroupDisplay(const char* Group)
{
    if (std::string_view(Group) == kGroupTask) return L("任务道具", "Quest Items");
    if (std::string_view(Group) == kGroupPhys) return L("物理杂物", "Physics Props");
    return Group;
}

inline bool IsTaskGroup(const char* Group) { return std::string_view(Group) == kGroupTask; }
inline bool IsPhysGroup(const char* Group) { return std::string_view(Group) == kGroupPhys; }

} // namespace Lang
