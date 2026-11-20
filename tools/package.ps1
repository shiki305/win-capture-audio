#Requires -Version 5.1
<#
.SYNOPSIS
    Creates the distributable zip from the release layout.

.EXAMPLE
    .\tools\package.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root "release"
$dist = Join-Path $root "dist"

if (-not (Test-Path (Join-Path $release "obs-plugins\64bit\win-capture-audio.dll"))) {
    throw "Release layout not found. Run tools\build.ps1 first."
}

$version = (Get-Content (Join-Path $root "buildspec.json") -Raw | ConvertFrom-Json).version
$zip = Join-Path $dist "win-capture-audio-$version-win64.zip"

New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }

Compress-Archive -Path (Join-Path $release "*") -DestinationPath $zip

Write-Host "Package: $zip"
