<#
  SwanStation Xbox - installs the package on this PC (Windows 10/11 with Developer Mode on), to try it before the console.
  SPDX-License-Identifier: GPL-3.0-or-later

    .\tools\install-pc.ps1          (run PowerShell as administrator: the certificate goes to the machine's trusted people)

  The same package runs on a PC: Mesa Dozen uses the PC's Direct3D 12 GPU, an Xbox controller (or any XInput pad) plays.
  There is no HDMI display information on a PC, so the TV-mode requests (4K, 120 Hz) do nothing there.
  The app's folder is %LOCALAPPDATA%\Packages\SwanStationXbox_*\LocalState\SwanStation (games, BIOS, log).
#>
#requires -Version 5.1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$package = Get-ChildItem -Path (Join-Path $root "dist") -Include "SwanStationXbox_*_x64*.msix", "SwanStationXbox_*_x64*.appx" -Recurse |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $package) { throw "No .msix in dist: run tools\build-xbox.ps1 first." }

# the build leaves the signing certificate next to the package: Windows installs only what a trusted certificate signed
$cer = Get-ChildItem -Path $package.DirectoryName -Filter "*.cer" | Select-Object -First 1
if (-not $cer) { throw "No .cer next to the package: run tools\build-xbox.ps1 again." }
Import-Certificate -FilePath $cer.FullName -CertStoreLocation "Cert:\LocalMachine\TrustedPeople" | Out-Null

# the VC++ runtime the package depends on (x64 is the one that matters; the build copies it next to the package)
$deps = @(Get-ChildItem -Path (Join-Path $package.DirectoryName "Dependencies\x64") -Filter "*.appx" -ErrorAction SilentlyContinue | ForEach-Object FullName)
if ($deps.Count -gt 0) {
    Add-AppxPackage -Path $package.FullName -DependencyPath $deps -ForceApplicationShutdown
} else {
    Add-AppxPackage -Path $package.FullName -ForceApplicationShutdown
}
Write-Host "installed: start 'SwanStation Xbox' from the Start menu."
Write-Host "log: $env:LOCALAPPDATA\Packages\SwanStationXbox_*\LocalState\SwanStation\logs\SwanStationPS5.log"
