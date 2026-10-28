# Dual-language packaging: build CN + EN menus, output dist\{LANG}.EvilNunModMenu.dll
# Usage: powershell -File package_langs.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$jobs = @(
    @{ Lang = "CN"; Flag = "-DMODMENU_LANG_EN=OFF" },
    @{ Lang = "EN"; Flag = "-DMODMENU_LANG_EN=ON"  }
)

foreach ($j in $jobs) {
    $lang = $j.Lang
    $buildDir = "build_" + $lang
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }

    Write-Output ("==== [" + $lang + "] configure " + $j.Flag + " ====")
    & cmake -S . -B $buildDir -G Ninja $j.Flag
    if ($LASTEXITCODE -ne 0) { throw ("[" + $lang + "] configure failed") }

    Write-Output ("==== [" + $lang + "] build ====")
    & cmake --build $buildDir --parallel 8
    if ($LASTEXITCODE -ne 0) { throw ("[" + $lang + "] build failed") }

    New-Item -ItemType Directory -Force -Path dist | Out-Null
    $src = Join-Path $buildDir "EvilNunModMenu.dll"
    $dst = "dist\" + $lang + ".EvilNunModMenu.dll"
    Copy-Item -Force $src $dst
    Write-Output ("[" + $lang + "] -> " + $dst)
}

# 自检: 两份 DLL 必须字节不同, 且各自含正确语言的界面字符串
# (中文串用 Unicode 码点构造, 避免 PowerShell 5.1 按 ANSI 读无 BOM 脚本导致字面量乱码)
$py = "C:\Users\ace20\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
& $py -c @"
cn = open('dist/CN.EvilNunModMenu.dll','rb').read()
en = open('dist/EN.EvilNunModMenu.dll','rb').read()
assert cn != en, 'FAIL: CN/EN identical'
god_cn = ''.join(map(chr, [0x4E0A, 0x5E1D, 0x6A21, 0x5F0F])).encode('utf-8')  # shang-di-mo-shi
god_en = b'God Mode'
assert cn.count(god_cn) and not cn.count(god_en), 'FAIL: CN strings'
assert en.count(god_en) and not en.count(god_cn), 'FAIL: EN strings'
print('OK: CN/EN verified distinct')
"@
if ($LASTEXITCODE -ne 0) { throw "语言自检失败" }

# 按 dist 惯例打日期 zip 包 (含两份 DLL)
$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$zip = "dist\EvilNunModMenu_$stamp.zip"
Compress-Archive -Path "dist\CN.EvilNunModMenu.dll","dist\EN.EvilNunModMenu.dll" -DestinationPath $zip -Force
Write-Output ("zip -> " + $zip)

Write-Output ""
Get-ChildItem dist\*.EvilNunModMenu.dll, dist\*_$stamp.zip | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
