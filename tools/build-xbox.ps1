<#
  SwanStationUWP - builds the UWP package for the Xbox Series X|S: dist\SwanStationXbox_<version>_x64.appx (+ the certificate).
  SPDX-License-Identifier: GPL-3.0-or-later

    .\tools\build-xbox.ps1 [-Configuration Release] [-SkipCore]

  Steps: the tile images, Mesa Dozen, the core, the NuGet package (C++/WinRT), MSBuild with signing. Run from a
  "x64 Native Tools Command Prompt for VS 2022" or any PowerShell with MSBuild on the path (the script finds it).
#>
#requires -Version 5.1
param(
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [switch]$SkipCore
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$project = Join-Path $root "xbox\SwanStationXbox.vcxproj"

function Find-MSBuild {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -version "[17.0,18.0)" -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) { return $found }
    }
    throw "MSBuild was not found. Install Visual Studio 2022 with the 'Universal Windows Platform development' workload."
}

# the temporary signing key: the package's publisher must match its subject (Package.appxmanifest)
$pfx = Join-Path $root "xbox\SwanStationXbox_TemporaryKey.pfx"
if (-not (Test-Path $pfx)) {
    Write-Host "making the signing certificate"
    # 10 years: the default is one, and a package signed with an expired certificate cannot be installed or updated
    $cert = New-SelfSignedCertificate -Type Custom -Subject "CN=SwanStationXbox" -KeyUsage DigitalSignature -FriendlyName "SwanStationUWP" `
        -NotAfter (Get-Date).AddYears(10) -CertStoreLocation "Cert:\CurrentUser\My" -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    $password = ConvertTo-SecureString -String "swanstation" -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $password | Out-Null
    Export-Certificate -Cert $cert -FilePath (Join-Path $root "xbox\SwanStationXbox.cer") | Out-Null
}

python (Join-Path $root "tools\make-xbox-assets.py")
& (Join-Path $root "tools\fetch-mesa.ps1")
if (-not $SkipCore) { & (Join-Path $root "tools\build-core.ps1") -Configuration $Configuration }
if (-not (Test-Path (Join-Path $root "xbox\Core\x64\swanstation_libretro.dll"))) {
    throw "xbox\Core\x64\swanstation_libretro.dll is missing: run tools\build-core.ps1."
}

$msbuild = Find-MSBuild
& $msbuild $project -t:restore -p:RestorePackagesConfig=true -p:Configuration=$Configuration -p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "the NuGet restore failed." }
& $msbuild $project -p:Configuration=$Configuration -p:Platform=x64 -p:AppxBundle=Never -p:UapAppxPackageBuildMode=SideloadOnly `
    -p:AppxPackageSigningEnabled=true -p:PackageCertificateKeyFile=$pfx -p:PackageCertificatePassword=swanstation -m
if ($LASTEXITCODE -ne 0) { throw "the build failed." }

Write-Host "done: $(Join-Path $root 'dist')"
