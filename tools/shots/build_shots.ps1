# 构建并运行界面截图工具 (CN + EN 两版), 输出 PNG 到 docs/images
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
$imgui = Join-Path $root "deps\imgui"
$out   = Join-Path $root "build_shots"
$docs  = Join-Path $root "docs\images"
New-Item -ItemType Directory -Force -Path $out, $docs | Out-Null

$sources = @(
    "$PSScriptRoot\main.cpp",
    (Join-Path $imgui "imgui.cpp"),
    (Join-Path $imgui "imgui_draw.cpp"),
    (Join-Path $imgui "imgui_tables.cpp"),
    (Join-Path $imgui "imgui_widgets.cpp"),
    (Join-Path $imgui "backends\imgui_impl_win32.cpp"),
    (Join-Path $imgui "backends\imgui_impl_dx11.cpp")
)
$inc = @("-I$(Join-Path $root 'src')", "-I$imgui", "-I$(Join-Path $imgui 'backends')")

foreach ($lang in @("CN","EN")) {
    $defs = @("-DUNICODE","-D_UNICODE","-D_WIN32_WINNT=0x0A00")
    if ($lang -eq "EN") { $defs += "-DMODMENU_LANG_EN" }
    $cxx = @("-std=gnu++20","-O2","-municode","-static","-Wno-unused-variable","-Wno-format") +
           $defs + $inc + $sources +
           @("-ld3d11","-ldxgi","-ld3dcompiler","-lgdi32","-luser32","-ldwmapi","-o", (Join-Path $out "shots_$lang.exe"))
    Write-Output "==== build shots_$lang ===="
    & "D:\mingw64\bin\g++.exe" @cxx
    if ($LASTEXITCODE -ne 0) { throw "shots_$lang compile failed" }
}

$shots = Join-Path $out "shots"
New-Item -ItemType Directory -Force -Path $shots | Out-Null
foreach ($lang in @("CN","EN")) {
    Write-Output "==== run shots_$lang ===="
    & (Join-Path $out "shots_$lang.exe") $shots
}

# BMP -> PNG
$py = "C:\Users\ace20\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
& $py -c @"
from PIL import Image
import os, glob
src = r'$shots'; dst = r'$docs'
os.makedirs(dst, exist_ok=True)
for f in sorted(glob.glob(os.path.join(src, '*.bmp'))):
    im = Image.open(f).convert('RGB')
    name = os.path.basename(f)[:-4] + '.png'
    im.save(os.path.join(dst, name))
    print('png:', name, im.size)
"@
Write-Output "done -> $docs"
