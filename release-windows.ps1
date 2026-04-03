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
$ProgressPreference = "SilentlyContinue"

function Invoke-NoProfileSelf {
    param(
        [hashtable]$BoundParameters
    )

    if ($env:MPT_NO_PROFILE_REEXEC -eq '1') {
        return
    }

    $shellPath = if ($PSVersionTable.PSEdition -eq 'Core') {
        Join-Path $PSHOME 'pwsh.exe'
    } else {
        Join-Path $PSHOME 'powershell.exe'
    }

    if (-not (Test-Path $shellPath)) {
        return
    }

    $env:MPT_NO_PROFILE_REEXEC = '1'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    foreach ($entry in $BoundParameters.GetEnumerator()) {
        $argList += "-$($entry.Key)"
        if ($entry.Value -isnot [switch] -and $entry.Value -isnot [System.Management.Automation.SwitchParameter]) {
            $argList += [string]$entry.Value
            continue
        }
        if (-not [bool]$entry.Value) {
            $argList = $argList[0..($argList.Count - 2)]
        }
    }

    & $shellPath @argList
    exit $LASTEXITCODE
}

Invoke-NoProfileSelf -BoundParameters $PSBoundParameters

function Test-ReleaseExists {
    param(
        [string]$GhPath,
        [string]$TagName
    )

    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process `
            -FilePath $GhPath `
            -ArgumentList @('release', 'view', $TagName) `
            -Wait `
            -PassThru `
            -NoNewWindow `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        return $process.ExitCode -eq 0
    } finally {
        Remove-Item $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

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

function Get-ProjectVersion {
    param(
        [string]$Root
    )

    $cmakeListsPath = Join-Path $Root 'CMakeLists.txt'
    if (-not (Test-Path $cmakeListsPath)) {
        throw "CMakeLists.txt was not found: $cmakeListsPath"
    }

    $cmakeLists = Get-Content -Path $cmakeListsPath -Raw -Encoding utf8
    $match = [regex]::Match($cmakeLists, 'project\(MotionPngTuberPlayer VERSION ([^ )]+)')
    if (-not $match.Success) {
        throw "Could not determine MotionPngTuberPlayer version from $cmakeListsPath"
    }

    return $match.Groups[1].Value
}

function Get-VersionFromTag {
    param(
        [string]$TagName
    )

    if ($TagName -match '^[vV](.+)$') {
        return $Matches[1]
    }

    return $TagName
}

$ghCommand = Get-Command gh -ErrorAction Stop
$pluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectVersion = Get-ProjectVersion -Root $pluginRoot
$tagVersion = Get-VersionFromTag -TagName $Tag
if ($tagVersion -ne $projectVersion) {
    throw "Release tag '$Tag' does not match project version '$projectVersion'."
}

$buildDir = Join-Path $pluginRoot 'build-win-fallback-vs'
$packageScript = Join-Path $pluginRoot 'package-release.ps1'
$packageName = 'MotionPngTuberPlayer-obs-plugin-windows-x64'
$zipBaseName = "$packageName-$projectVersion"
$zipPath = Join-Path $pluginRoot "dist\$zipBaseName.zip"
$hashPath = Join-Path $pluginRoot "dist\$zipBaseName.sha256.txt"
$legacyHashPath = Join-Path $pluginRoot "dist\$packageName.sha256.txt"
if ($legacyHashPath -ne $hashPath -and (Test-Path $legacyHashPath)) {
    Remove-Item -Path $legacyHashPath -Force
}

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
if (-not (Test-Path $zipPath)) {
    throw "Expected package archive was not created: $zipPath"
}

$hash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $hashPath -Value "$hash  $zipBaseName.zip" -Encoding ascii

git rev-parse -q --verify "refs/tags/$Tag" *> $null
if ($LASTEXITCODE -ne 0) {
    git tag -a $Tag -m "MotionPngTuberPlayer $Tag"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

git push origin $Tag
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$releaseNotes = 'Windows x64 package release.'
$assetZip = "$zipPath#MotionPngTuberPlayer Windows x64 package"
$assetHash = "$hashPath#SHA256 checksum"

if (Test-ReleaseExists -GhPath $ghCommand.Source -TagName $Tag) {
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
