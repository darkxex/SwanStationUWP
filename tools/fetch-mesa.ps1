<#
  SwanStation Xbox - fetches the Mesa Dozen build (Vulkan on Direct3D 12 for UWP) into xbox\Mesa\x64.
  SPDX-License-Identifier: GPL-3.0-or-later

  The DLLs are SternXD's UWP fork of Mesa (https://github.com/SternXD/mesa-uwp, branch uwp-25.3.3), as shipped by
  Misunderstood-Wookiee/RetroArch-UWP (MesaBuild branch). They are taken from that repository at a fixed commit and
  checked against the hashes below; Mesa is MIT-licensed.

    .\tools\fetch-mesa.ps1
#>
#requires -Version 5.1
$ErrorActionPreference = "Stop"

$commit = "e015291058bac570ae5624252f847439eedb9047"
$base = "https://raw.githubusercontent.com/Misunderstood-Wookiee/RetroArch-UWP/$commit/pkg/msvc-uwp/RetroArch-msvcUWP/MESA/x64"
$files = [ordered]@{
    "vulkan_dzn.dll"    = "972c65469440af3c7834a84cd2b55c87457e7daa2e1bc660ac26180c01c2d272"
    "dxil.dll"          = "0daf173c97c81641632976de55ff6a7a5259799f34a2bc46edc9d71386f38053"
    "spirv_to_dxil.dll" = "91ca8420cf45c225a99e1c2364d90bddc4ea480fdec33599f3f81caab8238ddb"
    "z-1.dll"           = "8ba6c43146f261fc3c178b7177a1c59631063f55d62429c9d17ecef168a38d36"
}

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root "xbox\Mesa\x64"
New-Item -ItemType Directory -Force -Path $out | Out-Null

foreach ($name in $files.Keys) {
    $target = Join-Path $out $name
    if ((Test-Path $target) -and ((Get-FileHash $target -Algorithm SHA256).Hash -ieq $files[$name])) {
        Write-Host "ok      $name"
        continue
    }
    Write-Host "fetch   $name"
    Invoke-WebRequest -Uri "$base/$name" -OutFile $target -UseBasicParsing
    $hash = (Get-FileHash $target -Algorithm SHA256).Hash
    if ($hash -ine $files[$name]) {
        Remove-Item $target -Force
        throw "$name does not match the expected hash ($hash)."
    }
}
Write-Host "Mesa Dozen is in $out"
