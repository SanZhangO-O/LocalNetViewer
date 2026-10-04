# LocalNetViewer C++ server 构建脚本（Windows / MSVC）
# 用法: powershell -File build.ps1 [-Debug] [-Run] [-Clean]
param(
    [switch]$Debug,
    [switch]$Run,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# -Clean：先删掉 build/ 再构建。
# 为什么需要它：MSVC 的预编译头（cmake_pch.cxx.pch，本项目 ~350MB）在**构建被中断**时
# 可能只更新了一部分目标文件，剩下的仍按旧 PCH 编译 —— 于是同一个进程里不同 TU 看到的
# 结构体布局不一致（ODR 违规），表现为随机的堆损坏（0xc0000374 / 无效 free）。
# 实测：同一份源码在"被打断过的 build/"里并发压力测试必崩，干净重建后 120 轮无问题。
if ($Clean) {
    $bd = Join-Path $root "build"
    if (Test-Path $bd) { Remove-Item -Recurse -Force $bd; Write-Host "已清空 $bd" }
}

# 0. vendored 依赖自检（thirdparty 随仓库提交；缺失或补丁丢失时给出可执行的提示，
#    而不是让 CMake 抛一堆看不懂的错，或静默产出行为不一致的二进制）
$tp = Join-Path $root "thirdparty"
$required = @(
    "cpp-httplib\httplib.h", "json\single_include\nlohmann\json.hpp",
    "miniz\miniz.c", "miniz\miniz_zip.c", "stb\stb_image.h",
    "stb\stb_image_write.h", "stb\stb_image_resize2.h",
    "pugixml\CMakeLists.txt", "pugixml\src\pugixml.cpp",
    "yaml-cpp\CMakeLists.txt", "yaml-cpp\src",
    "freetype\CMakeLists.txt", "freetype\include\freetype\freetype.h",
    "freetype\builds\unix\freetype2.in"
)
$missing = @()
foreach ($rel in $required) {
    if (-not (Test-Path (Join-Path $tp $rel))) { $missing += $rel }
}
if ($missing.Count -gt 0) {
    throw ("thirdparty 不完整，缺少:`n  " + ($missing -join "`n  ") +
           "`n请重新获取依赖（见 server-cpp\thirdparty\README.md）")
}
$httplib = Join-Path $tp "cpp-httplib\httplib.h"
if (-not (Select-String -Path $httplib -Pattern "\[LNV PATCH\]" -Quiet)) {
    throw ("thirdparty\cpp-httplib\httplib.h 缺少 [LNV PATCH]（非法 Range 按 RFC 忽略）。`n" +
           "请应用 server-cpp\patches\cpp-httplib-lnv-range.patch，" +
           "否则非法 Range 会返回 416 而 Python 返回 200，两边行为不一致。")
}

# 1. 定位 Visual Studio（优先带 VC 工具的最新安装）
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -property installationPath | Select-Object -Last 1
}
if (-not $vsPath) {
    foreach ($p in @("C:\Program Files\Microsoft Visual Studio\18\Insiders",
                     "C:\Program Files\Microsoft Visual Studio\2022\Community")) {
        if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") { $vsPath = $p; break }
    }
}
if (-not $vsPath) { throw "未找到 Visual Studio（需要 MSVC C++ 工具集）" }
Write-Host "VS: $vsPath"

$vcvars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
$buildDir = Join-Path $root "build"
$cfg = if ($Debug) { "Debug" } else { "Release" }

$inner = @"
@echo off
cd /d "$buildDir" || exit /b 1
cmake -G Ninja -DCMAKE_BUILD_TYPE=$cfg .. || exit /b 1
cmake --build . --parallel || exit /b 1
"@
$innerFile = Join-Path $env:TEMP "lnv_build_$PID.cmd"
[System.IO.File]::WriteAllText($innerFile, $inner)
New-Item -ItemType Directory -Force $buildDir | Out-Null
$allArgs = "/c call `"$vcvars`" >nul 2>&1 && `"$innerFile`""
$proc = Start-Process -FilePath cmd.exe -ArgumentList $allArgs -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$buildDir\build.log" -RedirectStandardError "$buildDir\build.err.log"
if ($proc.ExitCode -ne 0) {
    Get-Content "$buildDir\build.log" -Tail 40
    Get-Content "$buildDir\build.err.log" -Tail 40
    throw "构建失败 (exit $($proc.ExitCode))"
}
Write-Host "构建完成: $buildDir\lnvserver.exe"

if ($Run) {
    # 注意不要用自动变量 $args（它保存脚本的未绑定参数），否则 --config 会被丢掉
    $runArgs = @("--config", (Join-Path $root "..\server\config.yaml"))
    & "$buildDir\lnvserver.exe" @runArgs
}
