<div align="center">

# Evil Nun: The Broken Mask — Mod Menu

**A feature-rich ImGui cheat menu for *Evil Nun: The Broken Mask* (UE5)**

[![Platform](https://img.shields.io/badge/Platform-Windows-blue)](#-installation)
[![Language](https://img.shields.io/badge/Lang-C%2B%2B20-green)](#-building)
[![UI](https://img.shields.io/badge/UI-Dear%20ImGui-orange)](#-building)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

**English** | [简体中文](README.zh-CN.md)

</div>

---

An injector-based cheat menu for the horror puzzle game **Evil Nun: The Broken Mask** (UE5). Built on a Dumper-7 reverse-engineered SDK with a Dear ImGui overlay, rendered through a DXGI `Present` hook.

Ships in **two language builds** — `CN.EvilNunModMenu.dll` (Chinese) and `EN.EvilNunModMenu.dll` (English) — selected at compile time. Both builds are functionally identical; see [Releases](../../releases).

## ✨ Features

| Tab | Features |
|---|---|
| **Player** | God Mode (immune to all damage and kills), infinite stamina, health restore; walk/run/crouch speed override, unlock jump (the game ships no jump binding — the mod drives it), noclip fly (F4); grant the quest mask, set teleport charges |
| **Items** | Scans every pickable (auto-grouped: quest items / physics props), search filter with aliases; spawn any item in front of you at a chosen distance; spawned quest items get their interaction identity transferred so world triggers (fuse boxes, etc.) accept them; force-load all streaming sublevels to complete the list |
| **Nun AI** | Freeze / blind / deafen nuns; kill & revive through the native death flow (configurable death duration, permanent death); teleport nuns ↔ player in both directions; body-scale override; live status (chasing / calm) and perception parameters (speed / sight / hearing) |
| **Teleport** | Every room and area aggregated from the game's own navigation waypoints, plus spawn points, with live distances and one-click teleport (auto-loads the destination sublevel first) |
| **ESP** | Nun boxes (red chasing / orange calm / grey dead, labeled with type and state), item highlight (cyan quest / blue-grey physics), name filter and distance display; keeps working while the menu is closed |
| **Visual** | FOV override, gamma (night vision), enable native console cheats |
| **Object** | Pick any scene actor at the crosshair (F2): inspect name / class / position / distance, teleport to it, pull it to you, delete it |
| **World** | Game version, level, day counter, player flags; complete the current mission |

## ⌨️ Hotkeys

| Key | Action |
|---|---|
| `F1` | Toggle menu |
| `F2` | Pick object at crosshair |
| `F4` | Toggle noclip fly |
| `END` | Unload the mod |

## 🖼️ Screenshots

> The watermark in the top-left corner reports injection status; ESP keeps drawing while the menu is closed.

| Player | Items | Nun AI | Teleport |
|---|---|---|---|
| ![player](docs/images/EN_player.png) | ![items](docs/images/EN_items.png) | ![nun](docs/images/EN_nun.png) | ![teleport](docs/images/EN_teleport.png) |

| ESP | Visual | Object | World |
|---|---|---|---|
| ![esp](docs/images/EN_esp.png) | ![visual](docs/images/EN_visual.png) | ![object](docs/images/EN_object.png) | ![world](docs/images/EN_world.png) |

## 📥 Installation

1. Download the DLL for your language from [Releases](../../releases):
   - `EN.EvilNunModMenu.dll` — English menu
   - `CN.EvilNunModMenu.dll` — Chinese menu
2. Rename it to whatever your injector loads (for example `version.dll`);
3. Inject it into the game process (or drop it next to the game executable, depending on your method);
4. Press **F1** in-game to open the menu.

> The Chinese build renders with the system's Microsoft YaHei font, so no extra font file is needed. The English build loads the same CJK font, because in-game item names may themselves be Chinese.

## 🔨 Building

Requires **CMake ≥ 3.16 + Ninja + MinGW-w64 (g++ ≥ 13)**.

The game SDK (Dumper-7 output) is **not** included — it is large and breaks whenever the game updates. Dump *Evil Nun: The Broken Mask* yourself and place it next to this repository:

```
<workspace>/
├── dumper/CppSDK/     ← your own Dumper-7 SDK
└── EvilNun-.../       ← this repository
```

If you keep it elsewhere, adjust `SDK_DIR` in `CMakeLists.txt`.

```bash
# English build
cmake -S . -B build -G Ninja -DMODMENU_LANG_EN=ON
cmake --build build --parallel

# Chinese build (default)
cmake -S . -B build -G Ninja -DMODMENU_LANG_EN=OFF
cmake --build build --parallel
```

The result is `build/EvilNunModMenu.dll`, statically linked against the MinGW runtime — no external dependencies.

To produce both languages at once, with a self-check that the two binaries really differ:

```powershell
powershell -File package_langs.ps1     # -> dist/CN.*.dll, dist/EN.*.dll + dated zip
```

## 🏗️ Technical notes

- **Rendering** — hooks `IDXGISwapChain::Present` with auto-detected D3D11 / D3D12 backends and a safe rebuild on resize.
- **Input** — the WndProc hook translates window messages into events on a queue that the render thread drains (ImGui is not thread-safe, so nothing may touch it from the window thread). While the menu is open, game input is suppressed and the cursor is taken over; a `SetCursorPos` hook stops the game from snapping the pointer back to the window center every frame.
- **Stability** — level transitions trigger a GC destruction storm, so all game-object access is frozen for 3 seconds afterwards; every cached pointer is validated against its GObjects slot before use.
- **Language** — a compile-time switch (`lang.h` provides the `L("中文", "English")` macro). Internal group keys stay stable across both builds, so logic and logs are identical.

## ⚠️ Disclaimer

For personal study and research only. Use it offline / in single-player. You assume all risk, including bans. Not affiliated with Keone or the official Evil Nun franchise.
