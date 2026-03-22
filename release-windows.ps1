param(
    [Parameter(Mandatory = $true)]
    [string]$Tag,
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$PreRelease,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-CMakePath {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCommand) {
        return $cmakeCommand.Source
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

$ghCommand = Get-Command gh -ErrorAction Stop
$pluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $pluginRoot 'build-win-fallback-vs'
$packageScript = Join-Path $pluginRoot 'package-release.ps1'
$packageName = 'MotionPngTuberPlayer-obs-plugin-windows-x64'
$zipPath = Join-Path $pluginRoot "dist\$packageName.zip"
$hashPath = Join-Path $pluginRoot "dist\$packageName.sha256.txt"

Set-Location $pluginRoot

if (-not $SkipBuild) {
    $cmake = Get-CMakePath
    & $cmake -S $pluginRoot -B $buildDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cmake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& $packageScript -Configuration $Configuration -PackageName $packageName
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$hash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $hashPath -Value "$hash  $packageName.zip" -Encoding ascii

git rev-parse -q --verify "refs/tags/$Tag" *> $null
if ($LASTEXITCODE -ne 0) {
    git tag -a $Tag -m "MotionPngTuberPlayer $Tag"
}

git push origin $Tag
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$releaseNotes = 'Windows x64 package release.'
$assetZip = "$zipPath#MotionPngTuberPlayer Windows x64 package"
$assetHash = "$hashPath#SHA256 checksum"

& $ghCommand.Source release view $Tag *> $null
if ($LASTEXITCODE -eq 0) {
    & $ghCommand.Source release upload $Tag $assetZip $assetHash --clobber
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    $args = @(
        'release', 'create', $Tag,
        $assetZip,
        $assetHash,
        '--title', "MotionPngTuberPlayer $Tag",
        '--generate-notes',
        '--notes', $releaseNotes
    )
    if ($PreRelease) {
        $args += '--prerelease'
    }

    & $ghCommand.Source @args
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Released $Tag"
Write-Host "ZIP: $zipPath"
Write-Host "SHA256: $hashPath"
