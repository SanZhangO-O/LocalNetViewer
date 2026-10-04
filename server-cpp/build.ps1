# LocalNetViewer C++ server 构建脚本（Windows / MSVC）
# 用法: powershell -File build.ps1 [-Debug] [-Run] [-Clean] [-Jobs 16]
param(
    [switch]$Debug,
    [switch]$Run,
    [switch]$Clean,
    # 编译并发数；0 = 默认（Ninja 自动用满全部 CPU 核心，本来就是并行的）
    [int]$Jobs = 0
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
$cores = [Environment]::ProcessorCount
$parallel = if ($Jobs -gt 0) { "--parallel $Jobs" } else { "--parallel" }
Write-Host "编译并发: $(if ($Jobs -gt 0) { $Jobs } else { $cores }) (CPU 核心数)"

$inner = @"
@echo off
rem vcvars 也放进本批处理：PS 5.1 的 & 传参会给含空格/引号的复合命令行整体加引号
rem 且不转义内部引号，cmd 的引号剥除规则会把命令行弄乱（实测内层脚本静默不执行）。
call "$vcvars" >nul 2>&1 || (echo [LNV] vcvars64 init FAILED & exit /b 1)
cd /d "$buildDir" || exit /b 1
cmake -G Ninja -DCMAKE_BUILD_TYPE=$cfg .. 2>&1 || exit /b 1
cmake --build . $parallel 2>&1 || exit /b 1
"@
$innerFile = Join-Path $env:TEMP "lnv_build_$PID.cmd"
[System.IO.File]::WriteAllText($innerFile, $inner, [System.Text.Encoding]::ASCII)
New-Item -ItemType Directory -Force $buildDir | Out-Null
# PS 只把 .cmd 文件路径交给 cmd（路径不含空格时不加引号，实测可靠）：
# 输出原生实时显示到控制台，Tee 一份到日志；退出码用 $LASTEXITCODE。
# 批处理内部已把各命令的 stderr 并入 stdout（2>&1）；这里再把
# $ErrorActionPreference 临时降为 Continue，是防止残余 stderr 触发
# PS 5.1 "EAP=Stop + 原生命令 stderr" 的 NativeCommandError 终止错误。
$buildLog = Join-Path $buildDir "build.log"
$staleErr = Join-Path $buildDir "build.err.log"
if (Test-Path $staleErr) { Remove-Item $staleErr -Force }
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& cmd.exe /c $innerFile | Tee-Object -FilePath $buildLog
$buildExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($buildExit -ne 0) {
    throw "构建失败 (exit $buildExit)，完整日志: $buildLog"
}
Write-Host "构建完成: $buildDir\lnvserver.exe"

if ($Run) {
    # 注意不要用自动变量 $args（它保存脚本的未绑定参数），否则 --config 会被丢掉
    $runArgs = @("--config", (Join-Path $root "..\server\config.yaml"))
    & "$buildDir\lnvserver.exe" @runArgs
}
