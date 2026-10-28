<div align="center">

# Evil Nun: The Broken Mask — Mod 菜单

**《邪恶修女：破碎面具》(UE5) 功能完整的 ImGui 作弊菜单**

[![平台](https://img.shields.io/badge/平台-Windows-blue)](#-安装)
[![语言](https://img.shields.io/badge/语言-C%2B%2B20-green)](#-构建)
[![界面](https://img.shields.io/badge/界面-Dear%20ImGui-orange)](#-构建)
[![许可](https://img.shields.io/badge/许可-MIT-yellow)](LICENSE)

[English](README.md) | **简体中文**

</div>

---

为恐怖解谜游戏 **《Evil Nun: The Broken Mask（邪恶修女：破碎面具）》** 制作的注入式作弊菜单。基于 Dumper-7 逆向 SDK，Dear ImGui 绘制界面，通过 hook DXGI `Present` 渲染。

提供**中 / 英双语两个构建** —— `CN.EvilNunModMenu.dll`（中文）与 `EN.EvilNunModMenu.dll`（英文），编译期选择，功能完全一致；见 [Releases](../../releases)。

## ✨ 功能特性

| 页面 | 功能 |
|---|---|
| **玩家** | 上帝模式（免疫一切伤害与击杀）、无限体力、恢复生命；走路/跑步/下蹲速度修改、解锁跳跃（游戏原生未绑定跳跃，由 mod 驱动）、穿墙飞行（F4）；获得任务面具、设置传送点数 |
| **道具** | 扫描全部可拾取物（任务道具 / 物理杂物自动分组），支持搜索过滤与别名；按所选距离在玩家面前生成任意物品；生成的任务道具会转移交互身份，保险丝盒等场景交互可正常识别；一键强制加载全部流式子关卡以补全列表 |
| **修女 AI** | 冻结 / 失明 / 失聪修女；走原生死亡流程击杀与复活（死亡时长可调、可永久死亡）；修女与玩家双向传送；体型缩放；实时查看修女状态（追击 / 平静）与感知参数（速度 / 视野 / 听觉） |
| **传送** | 由游戏自带导航路点按官方分类聚合的全部房间/场景 + 出生点列表，显示实时距离，一键传送（自动先加载目标区域子关卡） |
| **ESP** | 修女方框（追击红 / 平静橙 / 死亡灰，标注种类与状态）、道具位置高亮（任务道具青色 / 物理杂物灰蓝），支持名称过滤与距离显示；菜单关闭时 ESP 依然工作 |
| **视觉** | FOV 修改、Gamma 夜视、启用游戏原生控制台作弊 |
| **对象** | 准星选取任意场景对象（F2）：查看名称/类/位置/距离，传送过去、拉到面前、删除 |
| **世界** | 游戏版本、关卡、天数、玩家状态查询；一键完成当前任务 |

## ⌨️ 快捷键

| 按键 | 功能 |
|---|---|
| `F1` | 显示 / 隐藏菜单 |
| `F2` | 选取视线中心对象 |
| `F4` | 切换穿墙飞行 |
| `END` | 卸载 Mod |

## 🖼️ 界面截图

> 左上角水印显示注入状态；菜单关闭时 ESP 依然在绘制。

| 玩家 | 道具生成 | 修女 AI | 传送 |
|---|---|---|---|
| ![player](docs/images/CN_player.png) | ![items](docs/images/CN_items.png) | ![nun](docs/images/CN_nun.png) | ![teleport](docs/images/CN_teleport.png) |

| ESP | 视觉 | 对象选取 | 世界 |
|---|---|---|---|
| ![esp](docs/images/CN_esp.png) | ![visual](docs/images/CN_visual.png) | ![object](docs/images/CN_object.png) | ![world](docs/images/CN_world.png) |

## 📥 安装

1. 从 [Releases](../../releases) 下载对应语言的 DLL：
   - `CN.EvilNunModMenu.dll` — 中文菜单
   - `EN.EvilNunModMenu.dll` — 英文菜单
2. 重命名为你的注入器所加载的文件名（例如 `version.dll`）；
3. 注入游戏进程（或直接放入游戏可执行文件旁，视注入方式而定）；
4. 进入游戏后按 **F1** 打开菜单。

> 中文版使用系统自带的微软雅黑渲染，无需额外字体文件。英文版同样加载 CJK 字体，因为游戏内物品名本身可能就是中文。

## 🔨 构建

依赖：**CMake ≥ 3.16 + Ninja + MinGW-w64 (g++ ≥ 13)**。

仓库**不包含**游戏 SDK（Dumper-7 输出，体积大且随游戏版本失效）。请自行 dump 一份《Evil Nun: The Broken Mask》的 CppSDK，放到与本仓库平级的位置：

```
<你的工作区>/
├── dumper/CppSDK/     ← 自备的 Dumper-7 SDK
└── EvilNun-.../       ← 本仓库
```

若放在别处，修改 `CMakeLists.txt` 中的 `SDK_DIR`。

```bash
# 中文版 (默认)
cmake -S . -B build -G Ninja -DMODMENU_LANG_EN=OFF
cmake --build build --parallel

# 英文版
cmake -S . -B build -G Ninja -DMODMENU_LANG_EN=ON
cmake --build build --parallel
```

产物为 `build/EvilNunModMenu.dll`，已静态链接 MinGW 运行时，无外部依赖。

一条命令同时产出双语（自带"两份二进制必须不同"的语言自检）：

```powershell
powershell -File package_langs.ps1     # -> dist/CN.*.dll, dist/EN.*.dll + 日期 zip
```

## 🏗️ 技术要点

- **渲染**：hook `IDXGISwapChain::Present`，D3D11 / D3D12 后端自适应，窗口 resize 时安全重建。
- **输入**：WndProc hook 把窗口消息翻译成事件入队，由渲染线程消费（ImGui 非线程安全，绝不允许跨线程直写）。菜单打开期间拦截游戏输入并接管光标；hook `SetCursorPos` 阻止游戏每帧把指针拽回窗口中心。
- **稳定性**：换关卡 / 回主菜单会触发 GC 销毁风暴，此后 3 秒冻结一切游戏对象访问；所有缓存指针使用前一律做 GObjects 槽位校验。
- **多语言**：编译期切换（`lang.h` 提供 `L("中文", "English")` 宏）。物品分组内部键跨语言固定，两份构建的逻辑与日志完全一致。

## ⚠️ 免责声明

仅供个人学习与研究，请在**离线 / 单人模式**下使用，不要在联机环境使用。使用者自行承担封号等一切风险。本 Mod 与 Keone 及《Evil Nun》官方无任何关联。
