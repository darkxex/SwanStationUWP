<#
  SwanStation Xbox - builds the SwanStation core (libretro's DuckStation fork) as a UWP DLL: xbox\Core\x64\swanstation_libretro.dll.
  SPDX-License-Identifier: GPL-3.0-or-later

  Needs Visual Studio 2022 (UWP C++ tools) and CMake. The core's source is the submodule third_party\swanstation; the
  project's changes to it are two patches, applied once (the submodule itself stays pristine in git's eyes only until
  they are applied: run "git -C third_party/swanstation checkout ." to undo):
    tools\patches\SSPS5.patch   the frontend's options, the Vulkan renderer's additions, the SPIR-V cache
    tools\patches\SSXBOX.patch  executable memory for the recompiler through the UWP calls, no fastmem aliasing

    .\tools\build-core.ps1 [-Configuration Release]
#>
#requires -Version 5.1
param([ValidateSet("Release", "Debug")][string]$Configuration = "Release")
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$core = Join-Path $root "third_party\swanstation"
$build = Join-Path $root "build\core-xbox"
$out = Join-Path $root "xbox\Core\x64"

if (-not (Test-Path (Join-Path $core "CMakeLists.txt"))) {
    throw "SwanStation's source is missing: run 'git submodule update --init --recursive'."
}

foreach ($patch in @("SSPS5.patch", "SSXBOX.patch")) {
    $file = Join-Path $root "tools\patches\$patch"
    # already applied? (the reverse applies cleanly)
    git -C $core apply --ignore-whitespace --reverse --check $file 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "patch $patch: already applied"; continue }
    Write-Host "patch $patch: applying"
    git -C $core apply --ignore-whitespace $file
    if ($LASTEXITCODE -ne 0) { throw "$patch does not apply to the core's source." }
}

# The core is a DLL of a UWP package: CMake's WindowsStore system, the recompiler on, no OpenGL/EGL/D3D backends
cmake -S $core -B $build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 `
    -DBUILD_LIBRETRO_CORE=ON -DBUILD_SDL_FRONTEND=OFF -DBUILD_QT_FRONTEND=OFF -DBUILD_NOGUI_FRONTEND=OFF
if ($LASTEXITCODE -ne 0) { throw "cmake could not configure the core." }
cmake --build $build --config $Configuration --target swanstation_libretro --parallel
if ($LASTEXITCODE -ne 0) { throw "the core did not build." }

$dll = Get-ChildItem -Path $build -Filter "swanstation_libretro.dll" -Recurse | Select-Object -First 1
if (-not $dll) { throw "swanstation_libretro.dll was not produced." }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item $dll.FullName $out -Force
Write-Host "core: $out\swanstation_libretro.dll"
