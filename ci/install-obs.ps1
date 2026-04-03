param(
  [string]$ObsDllPath = 'C:\Program Files\obs-studio\bin\64bit\obs.dll'
)

$ErrorActionPreference = 'Stop'

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

function Install-ObsViaDirectDownload {
  $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/obsproject/obs-studio/releases/latest'
  $asset = $release.assets | Where-Object { $_.name -match 'Windows.*Installer.*\.exe$' } | Select-Object -First 1
  if (-not $asset) {
    throw 'Could not find an OBS Studio Windows installer asset in the latest release.'
  }

  $installer = Join-Path $env:RUNNER_TEMP 'OBS-Studio-Installer.exe'
  Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $installer
  $process = Start-Process -FilePath $installer -ArgumentList '/S' -Wait -PassThru
  if ($process.ExitCode -eq 0) {
    return $true
  }

  Write-Warning "Direct download install failed with exit code $($process.ExitCode)."
  return $false
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
