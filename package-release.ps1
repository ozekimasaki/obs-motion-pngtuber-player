param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$BuildDir = "",
    [ValidateSet("windows", "linux", "macos")]
    [string]$Platform = "",
    [string]$OutputDir = "",
    [switch]$NoZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-CMakePath {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCommand) {
        return $cmakeCommand.Source
    }

    if (-not $IsWindows) {
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

if ([string]::IsNullOrWhiteSpace($Platform)) {
    if ($IsWindows) {
        $Platform = 'windows'
    } elseif ($IsLinux) {
        $Platform = 'linux'
    } elseif ($IsMacOS) {
        $Platform = 'macos'
    } else {
        throw 'Could not infer package platform.'
    }
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    switch ($Platform) {
        'windows' { $BuildDir = 'build-win-fallback-vs' }
        'linux' { $BuildDir = 'build-linux-stub' }
        'macos' { $BuildDir = 'build-macos-stub' }
    }
}

$buildRoot = Join-Path $pluginRoot $BuildDir
if (-not (Test-Path $buildRoot)) {
    throw "Build directory was not found: $buildRoot"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $pluginRoot "dist\MotionPngTuberPlayer-$Platform"
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
