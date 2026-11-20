#Requires -Version 5.1
<#
.SYNOPSIS
    Installs the built plugin into an OBS Studio installation.

.DESCRIPTION
    Copies the plugin DLL and its locale data to the OBS root folder, backing up
    any previous win-capture-audio.dll. Must run elevated when OBS lives under
    Program Files.

.EXAMPLE
    .\tools\install-local.ps1
    .\tools\install-local.ps1 -ObsRoot "D:\OBS"
#>
[CmdletBinding()]
param(
    [string]$ObsRoot = "C:\Program Files\obs-studio"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root "release"

if (-not (Test-Path (Join-Path $release "obs-plugins\64bit\win-capture-audio.dll"))) {
    throw "Build output not found in '$release'. Run tools\build.ps1 first."
}

if (-not (Test-Path (Join-Path $ObsRoot "bin\64bit\obs64.exe"))) {
    throw "'$ObsRoot' does not look like an OBS installation (bin\64bit\obs64.exe missing)."
}

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running. Close it before installing."
}

$targetDll = Join-Path $ObsRoot "obs-plugins\64bit\win-capture-audio.dll"
$targetData = Join-Path $ObsRoot "data\obs-plugins\win-capture-audio"

if (Test-Path $targetDll) {
    $backup = "$targetDll.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Copy-Item $targetDll $backup
    Write-Host "Backed up previous DLL to $backup"
}

Copy-Item -Force (Join-Path $release "obs-plugins\64bit\win-capture-audio.dll") $targetDll
New-Item -ItemType Directory -Force -Path $targetData | Out-Null
Copy-Item -Recurse -Force (Join-Path $release "data\obs-plugins\win-capture-audio\*") $targetData

Write-Host ""
Write-Host "Installed to $ObsRoot"
Write-Host "Restart OBS and check the log for '[win-capture-audio] Version 2.4.0'."
