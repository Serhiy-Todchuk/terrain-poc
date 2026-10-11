<#
  Builds TerrainPOC.sln from the command line (same as building in Visual Studio).
  Usage: powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Config Release|Debug] [-Rebuild]
#>
param(
    [string]$Config = "Release",
    [switch]$Rebuild
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found" }
$target = if ($Rebuild) { "Rebuild" } else { "Build" }
& $msbuild (Join-Path $Root "TerrainPOC.sln") /m /nologo /v:minimal "/t:$target" "/p:Configuration=$Config" "/p:Platform=x64"
exit $LASTEXITCODE
