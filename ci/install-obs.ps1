param(
  [string]$ObsDllPath = 'C:\Program Files\obs-studio\bin\64bit\obs.dll'
)

$ErrorActionPreference = 'Stop'
$ObsInstallRoot = Split-Path (Split-Path (Split-Path $ObsDllPath -Parent) -Parent) -Parent

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
    return Invoke-RestMethod -Uri 'https://api.github.com/repos/obsproject/obs-studio/releases/latest' -Headers (Get-GitHubHeaders)
  } catch {
    Write-Warning "Failed to query the latest OBS Studio release: $($_.Exception.Message)"
    return $null
  }
}

function Test-ObsInstalled {
  return Test-Path $ObsDllPath
}

function Install-ObsViaChocolatey {
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

  $archive = Join-Path $env:RUNNER_TEMP 'OBS-Studio-Windows-x64.zip'
  $extractRoot = Join-Path $env:RUNNER_TEMP 'mpt-obs-direct'

  try {
    if (Test-Path $archive) {
      Remove-Item -Path $archive -Force
    }
    if (Test-Path $extractRoot) {
      Remove-Item -Path $extractRoot -Recurse -Force
    }

    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive -Headers (Get-DownloadHeaders)
    Expand-Archive -Path $archive -DestinationPath $extractRoot -Force

    $downloadedDll = Get-ChildItem -Path $extractRoot -Recurse -Filter obs.dll |
      Where-Object { $_.FullName -match '\\bin\\64bit\\obs\.dll$' } |
      Select-Object -First 1
    if (-not $downloadedDll) {
      Write-Warning 'Downloaded OBS zip did not contain bin\\64bit\\obs.dll.'
      return $false
    }

    $sourceRoot = Split-Path (Split-Path (Split-Path $downloadedDll.FullName -Parent) -Parent) -Parent
    if (Test-Path $ObsInstallRoot) {
      Remove-Item -Path $ObsInstallRoot -Recurse -Force
    }
    New-Item -Path $ObsInstallRoot -ItemType Directory -Force | Out-Null
    Copy-Item -Path (Join-Path $sourceRoot '*') -Destination $ObsInstallRoot -Recurse -Force
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

  $installer = Join-Path $env:RUNNER_TEMP 'OBS-Studio-Installer.exe'
  try {
    if (Test-Path $installer) {
      Remove-Item -Path $installer -Force
    }

    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $installer -Headers (Get-DownloadHeaders)
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
