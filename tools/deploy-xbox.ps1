<#
  SwanStation Xbox - installs the package on a console in developer mode, through the Device Portal.
  SPDX-License-Identifier: GPL-3.0-or-later

    .\tools\deploy-xbox.ps1 -Ip 192.168.1.50 [-Pin 123456]

  On the console: Dev Home -> developer mode on, "Remote Management" on; the IP is shown there. WinAppDeployCmd.exe is part of
  the Windows SDK. The certificate (xbox\SwanStationXbox.cer) is installed on the console by the Device Portal on first use:
  upload it under "Add" if the install says the signature is untrusted. Visual Studio can also deploy straight to the console
  (project properties -> Debugging -> Remote Machine) for a debug run.
#>
#requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [string]$Pin
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$package = Get-ChildItem -Path (Join-Path $root "dist") -Filter "SwanStationXbox_*_x64*.appx" -Recurse | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $package) { throw "No .appx in dist: run tools\build-xbox.ps1 first." }

$tool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\WinAppDeployCmd.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $tool) { throw "WinAppDeployCmd.exe was not found: install the Windows 10/11 SDK." }

$arguments = @("install", "-file", $package.FullName, "-ip", $Ip)
if ($Pin) { $arguments += @("-pin", $Pin) }
Write-Host "installing $($package.Name) on $Ip"
& $tool.FullName @arguments
if ($LASTEXITCODE -ne 0) { throw "the install failed." }
Write-Host "installed: open 'SwanStation Xbox' from My games & apps -> Dev Home."
