param(
  [string]$ObsDllPath = 'C:\Program Files\obs-studio\bin\64bit\obs.dll'
)

$ErrorActionPreference = 'Stop'
$DefaultObsInstallRoot = Split-Path (Split-Path (Split-Path $ObsDllPath -Parent) -Parent) -Parent
$PortableObsInstallRoot = if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP 'obs-studio' } else { $DefaultObsInstallRoot }
$script:ResolvedObsInstallRoot = $DefaultObsInstallRoot
$script:ResolvedObsDllPath = $ObsDllPath

function Set-ResolvedObsInstallRoot {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Root
  )

  $script:ResolvedObsInstallRoot = $Root
  $script:ResolvedObsDllPath = Join-Path $Root 'bin\64bit\obs.dll'
}

function Publish-ObsInstallRoot {
  Write-Host "Using OBS install root $script:ResolvedObsInstallRoot."
  if ($env:GITHUB_ENV) {
    Add-Content -Path $env:GITHUB_ENV -Value "OBS_INSTALL_ROOT=$script:ResolvedObsInstallRoot"
  }
}

function Get-GitHubHeaders {
  $headers = @{
    'Accept' = 'application/vnd.github+json'
    'User-Agent' = 'MotionPngTuberPlayer-CI'
  }

  if ($env:GITHUB_TOKEN) {
    $headers['Authorization'] = "Bearer $env:GITHUB_TOKEN"
  }

  return $headers
}

function Get-DownloadHeaders {
  return @{
    'User-Agent' = 'MotionPngTuberPlayer-CI'
  }
}

function Get-LatestObsRelease {
  try {
    Write-Host 'Querying latest OBS Studio release metadata from GitHub.'
    return Invoke-RestMethod -Uri 'https://api.github.com/repos/obsproject/obs-studio/releases/latest' -Headers (Get-GitHubHeaders)
  } catch {
    Write-Warning "Failed to query the latest OBS Studio release: $($_.Exception.Message)"
    return $null
  }
}

function Test-ObsInstalled {
  return Test-Path $script:ResolvedObsDllPath
}

function Install-ObsViaChocolatey {
  Set-ResolvedObsInstallRoot -Root $DefaultObsInstallRoot
  if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Host 'Chocolatey is not available on this runner.'
    return $false
  }

  choco install obs-studio --no-progress -y
  if ($LASTEXITCODE -eq 0) {
    return $true
  }

  Write-Warning "Chocolatey install failed with exit code $LASTEXITCODE."
  return $false
}

function Install-ObsViaWinget {
  Set-ResolvedObsInstallRoot -Root $DefaultObsInstallRoot
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host 'winget is not available on this runner.'
    return $false
  }

  winget install --id OBSProject.OBSStudio --exact --source winget --accept-package-agreements --accept-source-agreements --silent
  if ($LASTEXITCODE -eq 0) {
    return $true
  }

  Write-Warning "winget install failed with exit code $LASTEXITCODE."
  return $false
}

function Install-ObsViaZipAsset {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Release
  )

  $asset = $Release.assets | Where-Object { $_.name -match 'OBS-Studio-.*-Windows-x64\.zip$' } | Select-Object -First 1
  if (-not $asset) {
    Write-Warning 'Could not find an OBS Studio Windows x64 zip asset in the latest release.'
    return $false
  }

  Write-Host "Trying OBS zip asset $($asset.name)."
  $archive = Join-Path $env:RUNNER_TEMP 'OBS-Studio-Windows-x64.zip'
  $extractRoot = Join-Path $env:RUNNER_TEMP 'mpt-obs-direct'

  try {
    Set-ResolvedObsInstallRoot -Root $PortableObsInstallRoot
    if (Test-Path $archive) {
      Remove-Item -Path $archive -Force
    }
    if (Test-Path $extractRoot) {
      Remove-Item -Path $extractRoot -Recurse -Force
    }

    Write-Host "Downloading OBS zip to $archive."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive -Headers (Get-DownloadHeaders)
    Write-Host "Expanding OBS zip into $extractRoot."
    Expand-Archive -Path $archive -DestinationPath $extractRoot -Force

    $downloadedDll = Get-ChildItem -Path $extractRoot -Recurse -Filter obs.dll |
      Where-Object { $_.FullName -match '\\bin\\64bit\\obs\.dll$' } |
      Select-Object -First 1
    if (-not $downloadedDll) {
      Write-Warning 'Downloaded OBS zip did not contain bin\\64bit\\obs.dll.'
      return $false
    }

    $sourceRoot = Split-Path (Split-Path (Split-Path $downloadedDll.FullName -Parent) -Parent) -Parent
    Write-Host "Copying OBS runtime from $sourceRoot to $script:ResolvedObsInstallRoot."
    if (Test-Path $script:ResolvedObsInstallRoot) {
      Remove-Item -Path $script:ResolvedObsInstallRoot -Recurse -Force
    }
    New-Item -Path $script:ResolvedObsInstallRoot -ItemType Directory -Force | Out-Null
    Copy-Item -Path (Join-Path $sourceRoot '*') -Destination $script:ResolvedObsInstallRoot -Recurse -Force
    return (Test-ObsInstalled)
  } catch {
    Write-Warning "Direct zip install failed: $($_.Exception.Message)"
    return $false
  }
}

function Install-ObsViaInstallerAsset {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Release
  )

  $asset = $Release.assets | Where-Object { $_.name -match 'OBS-Studio-.*-Windows-x64-Installer\.exe$' } | Select-Object -First 1
  if (-not $asset) {
    Write-Warning 'Could not find an OBS Studio Windows x64 installer asset in the latest release.'
    return $false
  }

  Write-Host "Trying OBS installer asset $($asset.name)."
  $installer = Join-Path $env:RUNNER_TEMP 'OBS-Studio-Installer.exe'
  try {
    Set-ResolvedObsInstallRoot -Root $DefaultObsInstallRoot
    if (Test-Path $installer) {
      Remove-Item -Path $installer -Force
    }

    Write-Host "Downloading OBS installer to $installer."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $installer -Headers (Get-DownloadHeaders)
    Write-Host 'Launching OBS silent installer.'
    $process = Start-Process -FilePath $installer -ArgumentList '/S' -Wait -PassThru
    if ($process.ExitCode -eq 0) {
      return (Test-ObsInstalled)
    }

    Write-Warning "Direct installer install failed with exit code $($process.ExitCode)."
    return $false
  } catch {
    Write-Warning "Direct installer install failed: $($_.Exception.Message)"
    return $false
  }
}

function Install-ObsViaDirectDownload {
  $release = Get-LatestObsRelease
  if (-not $release) {
    return $false
  }

  if (Install-ObsViaZipAsset -Release $release) {
    return $true
  }

  Write-Warning 'Direct zip install did not produce obs.dll. Trying installer asset.'
  return Install-ObsViaInstallerAsset -Release $release
}

if (Test-ObsInstalled) {
  Write-Host 'OBS Studio is already installed.'
} else {
  Install-ObsViaChocolatey | Out-Null
  if (-not (Test-ObsInstalled)) {
    Write-Warning 'Chocolatey install did not produce obs.dll. Trying winget.'
    Install-ObsViaWinget | Out-Null
  }
  if (-not (Test-ObsInstalled)) {
    Write-Warning 'winget install did not produce obs.dll. Trying direct download.'
    Install-ObsViaDirectDownload | Out-Null
  }
}

if (-not (Test-ObsInstalled)) {
  throw 'obs.dll was not found after OBS Studio installation.'
}

Publish-ObsInstallRoot
$global:LASTEXITCODE = 0
