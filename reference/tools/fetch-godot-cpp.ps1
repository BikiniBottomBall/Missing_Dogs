# 拉取 godot-cpp（GDExtension 的 C++ 绑定）。
#
# godot-cpp 不打 tag 到每个 Godot 小版本，官方的兼容策略是"扩展声明
# compatibility_minimum，新版引擎向后兼容旧扩展"。当前最新分支是 4.5，
# 搭配 Godot 4.5+ 都能用。
#
# 用法：
#   powershell -File tools/fetch-godot-cpp.ps1
#   powershell -File tools/fetch-godot-cpp.ps1 -Version godot-4.4-stable
param(
    [string]$Version = "godot-4.5-stable",
    [string]$Dir = "third_party/godot-cpp"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root $Dir

if (Test-Path (Join-Path $target "CMakeLists.txt")) {
    Write-Host "godot-cpp 已存在：$target（如需更新请先删除该目录）"
    exit 0
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null

# 这台机器上 git 的 schannel 后端不可用（SEC_E_NO_CREDENTIALS），改用 openssl
$ssl = @("-c", "http.sslBackend=openssl")

Write-Host "克隆 godot-cpp @ $Version ..."
& git @ssl clone --depth 1 --branch $Version --recurse-submodules --shallow-submodules `
    https://github.com/godotengine/godot-cpp $target

# 某些环境下 `git submodule` 会因 signal pipe 失败（MSYS2 的 fork 问题），
# 这时再手工补一次 gdextension/ 子模块。
$header = Join-Path $target "gdextension/gdextension_interface.h"
if (-not (Test-Path $header)) {
    Write-Host "子模块未拉取，手工补 godot-headers ..."
    & git @ssl clone --depth 1 https://github.com/godotengine/godot-headers `
        (Join-Path $target "gdextension")
}

if (-not (Test-Path (Join-Path $target "gdextension/gdextension_interface.h"))) {
    throw "godot-cpp 拉取不完整：缺少 gdextension/gdextension_interface.h"
}

Write-Host "完成：$target"
Write-Host ""
Write-Host "接下来构建 GDExtension："
Write-Host "  cmake -S . -B build-gdext -G `"Visual Studio 18 2026`" -A x64 ``"
Write-Host "        -DGODOTCPP_TARGET=template_release -DMD_BUILD_GDEXTENSION=ON"
Write-Host "  cmake --build build-gdext --config Release"
