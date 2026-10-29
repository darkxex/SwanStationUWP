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
    "spirv_to_dxil.dll" = "91ca8420cf45c225a99e1c2364d90bddc4ea480fdec33599f3f81caab8238ddb"
    "z-1.dll"           = "8ba6c43146f261fc3c178b7177a1c59631063f55d62429c9d17ecef168a38d36"
}

# dxil.dll (the DXIL validator Dozen signs its shaders with) is not the one of that repository: its 1.7.2308 crashes
# on the first pipeline (an access violation inside the validator, on a PC and on any shader). DirectXShaderCompiler's
# 1.8.2502 release has the fix; its bin\x64\dxil.dll is taken from the release's zip (MIT/LLVM license).
$dxcZip = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2502/dxc_2025_02_20.zip"
$dxilHash = "62736e99aa534a942873f92b881a71b2cb6ad62b8d7481ea7a8ad8e9888a8056"

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
$dxil = Join-Path $out "dxil.dll"
if ((Test-Path $dxil) -and ((Get-FileHash $dxil -Algorithm SHA256).Hash -ieq $dxilHash)) {
    Write-Host "ok      dxil.dll"
}
else {
    Write-Host "fetch   dxil.dll (DirectXShaderCompiler 1.8.2502)"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipFile = Join-Path ([IO.Path]::GetTempPath()) "dxc_2025_02_20.zip"
    Invoke-WebRequest -Uri $dxcZip -OutFile $zipFile -UseBasicParsing
    $zip = [IO.Compression.ZipFile]::OpenRead($zipFile)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq "bin/x64/dxil.dll" } | Select-Object -First 1
        if (-not $entry) { throw "bin/x64/dxil.dll is not in the DirectXShaderCompiler release." }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dxil, $true)
    }
    finally { $zip.Dispose(); Remove-Item $zipFile -Force -ErrorAction SilentlyContinue }
    $hash = (Get-FileHash $dxil -Algorithm SHA256).Hash
    if ($hash -ine $dxilHash) {
        Remove-Item $dxil -Force
        throw "dxil.dll does not match the expected hash ($hash)."
    }
}
Write-Host "Mesa Dozen is in $out"
