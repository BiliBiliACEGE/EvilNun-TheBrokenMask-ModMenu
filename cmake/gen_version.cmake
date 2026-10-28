# 每次构建重新生成版本头 (秒级时间戳必变 → common.cpp 必重编译, 版本号永不陈旧)
file(MAKE_DIRECTORY "${OUT_DIR}")
string(TIMESTAMP STAMP "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT_DIR}/build_version.h"
"#pragma once
// 由构建系统每次构建时自动重新生成, 勿手动编辑
#define MODMENU_BUILD_STAMP \"${STAMP}\"
")
