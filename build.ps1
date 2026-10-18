# Builds build\Release\FileTools.exe with the newest Visual Studio found.
# Usage:  .\build.ps1            (Release)
#         .\build.ps1 -Config Debug
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio no encontrado (falta vswhere.exe)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Instala Visual Studio con la carga de trabajo 'Desarrollo para el escritorio con C++'." }

# vcpkg: VCPKG_ROOT if set, otherwise the copy bundled with Visual Studio.
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $vs "VC\vcpkg" }
$toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) { throw "No se encontró vcpkg en $vcpkgRoot. Define VCPKG_ROOT." }

cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build "$PSScriptRoot\build" --config $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "`nListo: $PSScriptRoot\build\$Config\FileTools.exe"
