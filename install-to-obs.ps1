param(
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$DataOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-RequiresAdministrator([string]$Path) {
    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    if ([string]::IsNullOrWhiteSpace($programFiles)) {
        return $false
    }

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullProgramFiles = [IO.Path]::GetFullPath($programFiles)
    return $fullPath.StartsWith($fullProgramFiles, [StringComparison]::OrdinalIgnoreCase)
}

$pluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$dllSource = Join-Path $pluginRoot "build-win-fallback-vs\$Configuration\MotionPngTuberPlayer.dll"
$dataSource = Join-Path $pluginRoot "data"
$obsExe64 = Join-Path $ObsRoot "bin\64bit\obs64.exe"
$obsDll64 = Join-Path $ObsRoot "bin\64bit\obs.dll"

$pluginBinDestination = Join-Path $ObsRoot "obs-plugins\64bit"
$pluginDataDestination = Join-Path $ObsRoot "data\obs-plugins\MotionPngTuberPlayer"

if (-not $DataOnly -and -not (Test-Path $dllSource)) {
    throw "Built DLL was not found: $dllSource"
}

if (-not (Test-Path $dataSource)) {
    throw "Plugin data directory was not found: $dataSource"
}

if (-not (Test-Path $obsExe64) -and -not (Test-Path $obsDll64)) {
    throw "Target OBS root does not look like a 64-bit OBS install: $ObsRoot"
}

if ((Test-RequiresAdministrator $pluginBinDestination) -or (Test-RequiresAdministrator $pluginDataDestination)) {
    if (-not (Test-IsAdministrator)) {
        throw "Installing into '$ObsRoot' requires Administrator PowerShell. Re-run this script as Administrator, or pass a writable OBS root such as a portable OBS folder."
    }
}

New-Item -ItemType Directory -Force -Path $pluginBinDestination | Out-Null
New-Item -ItemType Directory -Force -Path $pluginDataDestination | Out-Null

if (-not $DataOnly) {
    Copy-Item -Path $dllSource -Destination $pluginBinDestination -Force
}

Copy-Item -Path (Join-Path $dataSource "*") -Destination $pluginDataDestination -Recurse -Force

$installedDll = Join-Path $pluginBinDestination "MotionPngTuberPlayer.dll"
$installedJa = Join-Path $pluginDataDestination "locale\ja-JP.ini"
$installedEn = Join-Path $pluginDataDestination "locale\en-US.ini"

if (-not $DataOnly -and -not (Test-Path $installedDll)) {
    throw "DLL install verification failed: $installedDll"
}
if (-not (Test-Path $installedJa)) {
    throw "Japanese locale install verification failed: $installedJa"
}
if (-not (Test-Path $installedEn)) {
    throw "English locale install verification failed: $installedEn"
}

Write-Host "Installed MotionPngTuberPlayer to $ObsRoot"
if ($DataOnly) {
    Write-Host "Copied plugin data and locale files."
} else {
    Write-Host "Copied plugin DLL and data files."
}
Write-Host "Restart OBS to reload the plugin locale files."
