#Requires -Version 7.0
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$PSNativeCommandUseErrorActionPreference = $true

$root = $PSScriptRoot
$build = Join-Path $root 'build'

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw 'cmake.exe is not on PATH'
}

$configure = @(
    '-S', $root
    '-B', $build
    '-G', 'Visual Studio 17 2022'
    '-A', 'x64'
)
& cmake.exe @configure

$buildArgs = @(
    '--build', $build
    '--config', $Config
    '--parallel'
)
& cmake.exe @buildArgs

$exe = Join-Path $build "bin\$Config\localrdp.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build succeeded but $exe was not found"
}

Write-Host "Built $exe"
