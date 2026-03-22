param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$OutputDir = "",
    [switch]$NoZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$pluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$dllSource = Join-Path $pluginRoot "build-win-fallback-vs\$Configuration\MotionPngTuberPlayer.dll"
$dataSource = Join-Path $pluginRoot "data"

if (-not (Test-Path $dllSource)) {
    throw "Built DLL was not found: $dllSource"
}

if (-not (Test-Path $dataSource)) {
    throw "Plugin data directory was not found: $dataSource"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $pluginRoot "dist\MotionPngTuberPlayer-windows"
}

$packageRoot = [IO.Path]::GetFullPath($OutputDir)
$pluginBinDir = Join-Path $packageRoot "obs-plugins\64bit"
$pluginDataDir = Join-Path $packageRoot "data\obs-plugins\MotionPngTuberPlayer"

if (Test-Path $packageRoot) {
    Remove-Item -Path $packageRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $pluginBinDir | Out-Null
New-Item -ItemType Directory -Force -Path $pluginDataDir | Out-Null

Copy-Item -Path $dllSource -Destination $pluginBinDir -Force
Copy-Item -Path (Join-Path $dataSource "*") -Destination $pluginDataDir -Recurse -Force

$zipPath = "$packageRoot.zip"
if (Test-Path $zipPath) {
    Remove-Item -Path $zipPath -Force
}

if (-not $NoZip) {
    Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath -CompressionLevel Optimal
}

Write-Host "Packaged MotionPngTuberPlayer to $packageRoot"
Write-Host "DLL: $(Join-Path $pluginBinDir 'MotionPngTuberPlayer.dll')"
if (-not $NoZip) {
    Write-Host "ZIP: $zipPath"
}
