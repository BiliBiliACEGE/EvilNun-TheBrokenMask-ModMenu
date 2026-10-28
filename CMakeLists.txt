cmake_minimum_required(VERSION 3.16)
project(EvilNunModMenu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 菜单语言: OFF = 中文 (默认), ON = 英文 (打包脚本用 -DMODMENU_LANG_EN=ON 出英文版)
option(MODMENU_LANG_EN "Build the English menu" OFF)

# Dumper-7 SDK 路径
set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dumper/CppSDK")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook")

add_library(EvilNunModMenu SHARED
    # 项目源码
    src/dllmain.cpp
    src/common.cpp
    src/renderer.cpp
    src/game_context.cpp
    src/menu.cpp

    # Dear ImGui
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp

    # MinHook
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c

    # Dumper-7 SDK (只编译需要的包)
    ${SDK_DIR}/SDK/Basic.cpp
    ${SDK_DIR}/SDK/CoreUObject_functions.cpp
    ${SDK_DIR}/SDK/Engine_functions.cpp
    ${SDK_DIR}/SDK/EvilNun_Player_Character_functions.cpp
    ${SDK_DIR}/SDK/Character_Sister_Madelaine_Nun_functions.cpp
    ${SDK_DIR}/SDK/EvilNunGameInstance_functions.cpp
    ${SDK_DIR}/SDK/InspectObject_functions.cpp
    ${SDK_DIR}/SDK/EvilNunDoorActor_functions.cpp
    ${SDK_DIR}/SDK/GrailSpawnLocation_functions.cpp
    ${SDK_DIR}/SDK/CheckpointActor_functions.cpp
    ${SDK_DIR}/SDK/NunGrabObject_functions.cpp
    ${SDK_DIR}/SDK/AIModule_functions.cpp
    ${SDK_DIR}/SDK/AccessibilityPannel_functions.cpp
    ${SDK_DIR}/SDK/Fat_Nun_Philippa_Ai_Controller1_functions.cpp
    ${SDK_DIR}/SDK/Nun_Philippa_Character_Fat_chubby_functions.cpp
)

target_include_directories(EvilNunModMenu PRIVATE
    src
    ${SDK_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${MINHOOK_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)

# 每次构建刷新版本戳 (规避 __DATE__ 只在重编译 TU 更新的坑)
add_custom_target(VersionStamp ALL
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/build_version.h
    COMMAND ${CMAKE_COMMAND} -DOUT_DIR=${CMAKE_BINARY_DIR}/generated
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_version.cmake
    VERBATIM)
add_dependencies(EvilNunModMenu VersionStamp)

target_compile_definitions(EvilNunModMenu PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE
)

if(MODMENU_LANG_EN)
    target_compile_definitions(EvilNunModMenu PRIVATE MODMENU_LANG_EN)
endif()

# SDK 常见警告屏蔽 + GCC 兼容 (cmath 强制包含: SDK 使用 std::sqrt 但未 include)
target_compile_options(EvilNunModMenu PRIVATE
    -Wno-invalid-offsetof
    -Wno-attributes
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-format
)

target_link_libraries(EvilNunModMenu PRIVATE
    d3d11
    dxgi
    d3d12
    dxguid
    d3dcompiler
    dwmapi
    user32
    gdi32
)

# 静态链接 MinGW 运行库 (libgcc/libstdc++/winpthread),
# 否则游戏进程找不到这些 DLL 会导致 LoadLibrary 失败 (注入后无任何反应)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(EvilNunModMenu PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

set_target_properties(EvilNunModMenu PROPERTIES
    PREFIX ""
    OUTPUT_NAME "EvilNunModMenu"
)
