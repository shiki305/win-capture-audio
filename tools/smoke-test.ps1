<#
.SYNOPSIS
    Testa se o win-capture-audio.dll carrega com o obs.dll instalado e exporta
    os símbolos esperados, sem abrir o OBS.

.NOTES
    Gera o executável em build\smoke\ e o remove no final se -Keep não for usado.
#>
[CmdletBinding()]
param(
    [string]$ObsDll = "C:\Program Files\obs-studio\bin\64bit\obs.dll",
    [string]$PluginDll,
    [switch]$Keep
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
if (-not $PluginDll) {
    $PluginDll = Join-Path $Root "release\obs-plugins\64bit\win-capture-audio.dll"
}

if (-not (Test-Path $ObsDll)) { throw "obs.dll não encontrado: $ObsDll" }
if (-not (Test-Path $PluginDll)) { throw "plugin não encontrado: $PluginDll" }

$VsRoot = "D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$VcVars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) { throw "vcvars64.bat não encontrado: $VcVars" }

$OutDir = Join-Path $Root "build\smoke"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Exe = Join-Path $OutDir "smoke-test.exe"

$Src = Join-Path $PSScriptRoot "smoke-test.cpp"
$Bat = Join-Path $OutDir "build-smoke.bat"
# Os caminhos vão por variável de ambiente para o .bat não precisar carregar
# caracteres não-ASCII (o cmd lê o arquivo na code page do console).
$env:SMOKE_VCVARS = $VcVars
$env:SMOKE_OUT = $OutDir
$env:SMOKE_EXE = $Exe
$env:SMOKE_SRC = $Src
@(
    "@echo off",
    "call `"%SMOKE_VCVARS%`" >nul",
    "cl /nologo /EHsc /W4 /MD /Fo`"%SMOKE_OUT%\\`" /Fe`"%SMOKE_EXE%`" `"%SMOKE_SRC%`""
) | Set-Content -Encoding ascii $Bat
cmd /c "`"$Bat`"" 2>&1 | ForEach-Object { $_ }
if (-not (Test-Path $Exe)) { throw "falha ao compilar o smoke-test" }

& $Exe $ObsDll $PluginDll
$exit = $LASTEXITCODE

if (-not $Keep) {
    Remove-Item $Exe, (Join-Path $OutDir "smoke-test.obj"), $Bat -Force -ErrorAction SilentlyContinue
}

exit $exit
