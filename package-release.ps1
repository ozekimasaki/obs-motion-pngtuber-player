param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$BuildDir = "",
    [ValidateSet("windows")]
    [string]$Platform = "windows",
    [string]$PackageName = "",
    [string]$OutputDir = "",
    [switch]$NoZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsWindows {
    return $env:OS -eq 'Windows_NT'
}

function Get-CMakePath {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCommand) {
        return $cmakeCommand.Source
    }

    if (-not (Test-IsWindows)) {
        throw 'cmake was not found in PATH.'
    }

    $candidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw 'cmake.exe was not found.'
}

$pluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-IsWindows)) {
    throw 'package-release.ps1 currently supports Windows packaging only.'
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = 'build-win-fallback-vs'
}

$buildRoot = Join-Path $pluginRoot $BuildDir
if (-not (Test-Path $buildRoot)) {
    throw "Build directory was not found: $buildRoot"
}

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $PackageName = 'MotionPngTuberPlayer-obs-plugin-windows-x64'
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $pluginRoot "dist\$PackageName"
}

$packageRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path $packageRoot) {
    Remove-Item -Path $packageRoot -Recurse -Force
}

$cmake = Get-CMakePath
& $cmake --install $buildRoot --config $Configuration --prefix $packageRoot
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$zipPath = "$packageRoot.zip"
if (Test-Path $zipPath) {
    Remove-Item -Path $zipPath -Force
}

if (-not $NoZip) {
    Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal
}

Write-Host "Packaged MotionPngTuberPlayer to $packageRoot"
if (-not $NoZip) {
    Write-Host "ZIP: $zipPath"
}
