<#
  SwanStation Xbox - builds the SwanStation core (libretro's DuckStation fork) as a UWP DLL: xbox\Core\x64\swanstation_libretro.dll.
  SPDX-License-Identifier: GPL-3.0-or-later

  Needs Visual Studio 2022 (UWP C++ tools) and CMake. The core's source is the submodule third_party\swanstation; the
  project's changes to it are two patches, applied once and in this order (the submodule itself stays pristine in git's eyes only until
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

# Visual Studio 2022's own CMake: another one on the path (MSYS2's, devkitPro's) has no Visual Studio generators
function Find-CMake {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -version "[17.0,18.0)" -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" | Select-Object -First 1
        if ($found) { return $found }
    }
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "CMake was not found. Install Visual Studio 2022 with the 'C++ CMake tools for Windows' component."
}
$cmake = Find-CMake

if (-not (Test-Path (Join-Path $core "CMakeLists.txt"))) {
    throw "SwanStation's source is missing: run 'git submodule update --init --recursive'."
}

# (Windows PowerShell turns git's and cmake's warnings on stderr into terminating errors under "Stop": the exit codes decide)
$ErrorActionPreference = "Continue"
$patches = @("SSPS5.patch", "SSXBOX.patch")
# The patches touch the same files, so only the last one applied can be reversed: the latest whose reverse applies
# cleanly is where the stack stands, and it and the ones before it are already in
$done = -1
for ($i = $patches.Count - 1; $i -ge 0; $i--) {
    git -C $core apply --ignore-whitespace --reverse --check (Join-Path $root "tools\patches\$($patches[$i])") 2>$null
    if ($LASTEXITCODE -eq 0) { $done = $i; break }
}
for ($i = 0; $i -lt $patches.Count; $i++) {
    $patch = $patches[$i]
    $file = Join-Path $root "tools\patches\$patch"
    if ($i -le $done) { Write-Host "patch ${patch}: already applied"; continue }
    Write-Host "patch ${patch}: applying"
    git -C $core apply --ignore-whitespace --whitespace=nowarn $file
    if ($LASTEXITCODE -ne 0) { throw "$patch does not apply to the core's source." }
}

# The core is a DLL of a UWP package: CMake's WindowsStore system, the recompiler on, no OpenGL/EGL/D3D backends
# (a set system name is a cross build to CMake, which then leaves the processor empty: the core's CMakeLists needs it)
& $cmake -S $core -B $build -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_SYSTEM_NAME=WindowsStore" "-DCMAKE_SYSTEM_VERSION=10.0" "-DCMAKE_SYSTEM_PROCESSOR=AMD64" `
    "-DBUILD_LIBRETRO_CORE=ON" "-DBUILD_SDL_FRONTEND=OFF" "-DBUILD_QT_FRONTEND=OFF" "-DBUILD_NOGUI_FRONTEND=OFF"
if ($LASTEXITCODE -ne 0) { throw "cmake could not configure the core." }
# UWP projects build with /sdl, which makes the CRT's deprecations (strdup, fopen...) errors in the third-party code
$env:CL = "/D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE"
& $cmake --build $build --config $Configuration --target swanstation_libretro --parallel
$built = $LASTEXITCODE -eq 0
Remove-Item Env:CL
if (-not $built) { throw "the core did not build." }
$ErrorActionPreference = "Stop"

$dll = Get-ChildItem -Path $build -Filter "swanstation_libretro.dll" -Recurse | Select-Object -First 1
if (-not $dll) { throw "swanstation_libretro.dll was not produced." }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item $dll.FullName $out -Force
Write-Host "core: $out\swanstation_libretro.dll"
