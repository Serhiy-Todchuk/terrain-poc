<#
  Builds the third-party dependency of TerrainPOC: Cesium Native (and, through
  its bundled vcpkg helper, all of Cesium Native's own dependencies).

  The application itself is a plain Visual Studio solution (TerrainPOC.sln).
  Cesium Native only ships a CMake build, so this script drives that build once
  and installs headers + static libraries into deps\install (vcpkg-like layout:
  include\, lib\ for Release, debug\lib\ for Debug), which is what
  TerrainPOC.vcxproj consumes.

  It also fetches Dear ImGui (sources compiled directly by the app project) into
  third_party\imgui and copies a few headers of Cesium Native's vcpkg
  dependencies (earcut, zlib-ng) into third_party; -ImGuiOnly does just these
  two (seconds, no Cesium rebuild).

  Usage (from any PowerShell prompt):
      powershell -ExecutionPolicy Bypass -File scripts\build_deps.ps1 [-Configs Debug,Release] [-ImGuiOnly]
#>
param(
    [string[]]$Configs = @("Release", "Debug"),
    [string]$CesiumTag = "v0.65.0",
    [string]$ImGuiTag = "v1.92.9b",
    [switch]$ImGuiOnly
)

$ErrorActionPreference = "Stop"
$Root       = Split-Path -Parent $PSScriptRoot
$Deps       = Join-Path $Root "deps"
$CesiumSrc  = Join-Path $Deps "cesium-native"
$BuildDir   = Join-Path $Deps "cn-build"
$Triplets   = Join-Path $Deps "triplets"
$Install    = Join-Path $Deps "install"
$ThirdParty = Join-Path $Root "third_party"

New-Item -ItemType Directory -Force $Deps, $Triplets | Out-Null

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# Headers of private Cesium Native dependencies the app uses directly, copied
# from the vcpkg tree into third_party: earcut.hpp (building roofs) and the
# zlib-ng API (deflate in the HDF5 reader; the library is installed with Cesium
# Native as zlibstatic-ng.lib).
function Copy-VcpkgHeaders {
    $include = Get-ChildItem (Join-Path $Deps "ezvcpkg\*\installed\x64-windows-static-md\include") -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $include) { Write-Warning "vcpkg tree not found in $Deps\ezvcpkg (build Cesium Native first)"; return }
    $headers = [ordered]@{
        "mapbox\earcut.hpp"       = "mapbox\earcut.hpp"
        "zlib-ng.h"               = "zlib-ng\zlib-ng.h"
        "zconf-ng.h"              = "zlib-ng\zconf-ng.h"
        "zlib_name_mangling-ng.h" = "zlib-ng\zlib_name_mangling-ng.h"
    }
    foreach ($name in $headers.Keys) {
        $from = Join-Path $include.FullName $name
        if (-not (Test-Path $from)) { Write-Warning "$name not found in $($include.FullName)"; continue }
        $to = Join-Path $ThirdParty $headers[$name]
        New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
        Copy-Item $from $to -Force
    }
}

# --- Dear ImGui (UI panel) -----------------------------------------------------
# Plain sources (core + Win32/DX11 backends) compiled by TerrainPOC.vcxproj.
$ImGui = Join-Path $ThirdParty "imgui"
$imguiTagFile = Join-Path $ImGui ".tag"
if (-not (Test-Path $imguiTagFile) -or (Get-Content $imguiTagFile -Raw).Trim() -ne $ImGuiTag) {
    Step "Fetching Dear ImGui $ImGuiTag"
    if (Test-Path $ImGui) { Remove-Item -Recurse -Force $ImGui }
    git clone --depth 1 --branch $ImGuiTag https://github.com/ocornut/imgui.git $ImGui
    if ($LASTEXITCODE -ne 0) { throw "git clone imgui failed" }
    Set-Content -Encoding ascii $imguiTagFile $ImGuiTag
}
if ($ImGuiOnly) {
    Copy-VcpkgHeaders # also refreshes the headers of an existing dependency build
    Step "Done (Dear ImGui $ImGuiTag in $ImGui)"
    exit 0
}

# --- Tools -------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$vs2022 = & $vswhere -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
$vsAny  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Join-Path $vsAny "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) { throw "cmake not found (install CMake or the VS 'C++ CMake tools' component)" }
}
Write-Host "cmake : $cmake"
Write-Host "VS2022: $vs2022"
Write-Host "VS    : $vsAny"

if ($vs2022) {
    $generator = "Visual Studio 17 2022"
    # Make vcpkg build the dependency ports with the same compiler.
    $env:VCPKG_VISUAL_STUDIO_PATH = $vs2022
} else {
    $generator = "Visual Studio 18 2026"
    $env:VCPKG_VISUAL_STUDIO_PATH = $vsAny
}

# --- Sources -----------------------------------------------------------------
if (-not (Test-Path (Join-Path $CesiumSrc "CMakeLists.txt"))) {
    Step "Cloning cesium-native $CesiumTag"
    git clone --depth 1 --branch $CesiumTag --recurse-submodules --shallow-submodules https://github.com/CesiumGS/cesium-native.git $CesiumSrc
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}

# Pin the platform toolset to v143 so the libraries link with the v143 app project
# from both VS 2022 and VS 2026.
@"
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v143)
"@ | Set-Content -Encoding ascii (Join-Path $Triplets "x64-windows-static-md.cmake")

$env:EZVCPKG_BASEDIR = Join-Path $Deps "ezvcpkg"
$env:VCPKG_OVERLAY_TRIPLETS = $Triplets
$env:VCPKG_TRIPLET = "x64-windows-static-md"

# --- Configure ---------------------------------------------------------------
Step "Configuring cesium-native ($generator, v143)"
# Note: Cesium Native installs its headers to the configure-time
# CMAKE_INSTALL_PREFIX, so it must be set here (not only via --prefix).
& $cmake -S $CesiumSrc -B $BuildDir -G $generator -A x64 -T v143 `
    "-DCMAKE_INSTALL_PREFIX=$Install" `
    -DCESIUM_TESTS_ENABLED=OFF `
    -DCESIUM_ENABLE_CLANG_TIDY=OFF `
    -DCESIUM_DISABLE_CURL=ON `
    -DCESIUM_INSTALL_STATIC_LIBS=ON `
    -DCESIUM_INSTALL_HEADERS=ON
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

# --- Build + install ---------------------------------------------------------
foreach ($cfg in $Configs) {
    Step "Building cesium-native ($cfg)"
    & $cmake --build $BuildDir --config $cfg --parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build ($cfg) failed" }

    # Release -> deps\install\lib, Debug -> deps\install\debug\lib
    $prefix = if ($cfg -eq "Debug") { Join-Path $Install "debug" } else { $Install }
    Step "Installing cesium-native ($cfg) -> $prefix"
    & $cmake --install $BuildDir --config $cfg --prefix $prefix
    if ($LASTEXITCODE -ne 0) { throw "cmake install ($cfg) failed" }
}

Copy-VcpkgHeaders

Step "Done"
foreach ($cfg in $Configs) {
    $libDir = if ($cfg -eq "Debug") { Join-Path $Install "debug\lib" } else { Join-Path $Install "lib" }
    $libs = Get-ChildItem $libDir -Filter *.lib
    Write-Host ("{0}: {1} libraries in {2}" -f $cfg, $libs.Count, $libDir)
}
