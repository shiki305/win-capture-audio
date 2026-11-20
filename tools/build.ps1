#Requires -Version 5.1
<#
.SYNOPSIS
    Builds win-capture-audio against the local OBS SDK.

.DESCRIPTION
    Convenience wrapper around CMake. Run tools/prepare-sdk.ps1 once first.

.EXAMPLE
    .\tools\build.ps1 -Config Release
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $root "deps\sdk\include\obs.h"))) {
    Write-Host "OBS SDK not found. Preparing it now..."
    & (Join-Path $PSScriptRoot "prepare-sdk.ps1")
}

if ($Clean -and (Test-Path (Join-Path $root "build"))) {
    Remove-Item -Recurse -Force (Join-Path $root "build")
}

Push-Location $root
try {
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    cmake --build build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
} finally {
    Pop-Location
}

$dll = Join-Path $root "build\$Config\win-capture-audio.dll"
Write-Host ""
Write-Host "Built: $dll"
Write-Host "Portable layout: $(Join-Path $root 'release')"
