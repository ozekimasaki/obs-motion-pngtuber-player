param(
  [switch]$NoInstall
)

$ErrorActionPreference = 'Stop'

function Get-FfmpegPath {
  $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue
  if ($ffmpeg) {
    return $ffmpeg
  }

  $ffmpegCandidates = @('C:\ProgramData\chocolatey\bin\ffmpeg.exe', 'C:\ffmpeg\bin\ffmpeg.exe')
  if ($env:ChocolateyInstall) {
    $ffmpegCandidates += (Join-Path $env:ChocolateyInstall 'bin\ffmpeg.exe')
  }
  if ($env:LOCALAPPDATA) {
    $ffmpegCandidates += (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ffmpeg.exe')
    $wingetPackageRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $wingetPackageRoot) {
      $wingetPackages = Get-ChildItem -Path $wingetPackageRoot -Directory -Filter 'Gyan.FFmpeg_*' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
      foreach ($wingetPackage in $wingetPackages) {
        $ffmpegCandidates += Get-ChildItem -Path $wingetPackage.FullName -Recurse -File -Filter 'ffmpeg.exe' -ErrorAction SilentlyContinue |
          Select-Object -ExpandProperty FullName
      }
    }
  }
  if ($env:RUNNER_TEMP) {
    $downloadRoot = Join-Path $env:RUNNER_TEMP 'mpt-ffmpeg'
    if (Test-Path $downloadRoot) {
      $ffmpegCandidates += Get-ChildItem -Path $downloadRoot -Recurse -File -Filter 'ffmpeg.exe' -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName
    }
  }

  return $ffmpegCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}

function Install-FfmpegViaChocolatey {
  if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Host 'Chocolatey is not available on this runner.'
    return $false
  }

  choco install ffmpeg --no-progress -y
  if ($LASTEXITCODE -eq 0) {
    return $true
  }

  Write-Warning "Chocolatey install failed with exit code $LASTEXITCODE."
  return $false
}

function Install-FfmpegViaWinget {
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host 'winget is not available on this runner.'
    return $false
  }

  winget install --id Gyan.FFmpeg --exact --source winget --accept-package-agreements --accept-source-agreements --silent
  if ($LASTEXITCODE -eq 0) {
    return $true
  }

  Write-Warning "winget install failed with exit code $LASTEXITCODE."
  return $false
}

function Install-FfmpegViaDirectDownload {
  $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/GyanD/codexffmpeg/releases/latest'
  $asset = $release.assets | Where-Object { $_.name -match '^ffmpeg-.*-essentials_build\.zip$' } | Select-Object -First 1
  if (-not $asset) {
    throw 'Could not find a Windows ffmpeg essentials zip asset in the latest GyanD/codexffmpeg release.'
  }

  $zipPath = Join-Path $env:RUNNER_TEMP 'ffmpeg-essentials.zip'
  $extractRoot = Join-Path $env:RUNNER_TEMP 'mpt-ffmpeg'
  if (Test-Path $extractRoot) {
    Remove-Item -Path $extractRoot -Recurse -Force
  }

  Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath
  Expand-Archive -Path $zipPath -DestinationPath $extractRoot -Force
  return $true
}

$ffmpeg = Get-FfmpegPath
if (-not $ffmpeg -and $NoInstall) {
  throw 'ffmpeg.exe was not found and installation is disabled.'
}

if (-not $ffmpeg) {
  Install-FfmpegViaChocolatey | Out-Null
  $ffmpeg = Get-FfmpegPath
}
if (-not $ffmpeg) {
  Write-Warning 'Chocolatey install did not produce ffmpeg.exe. Trying winget.'
  Install-FfmpegViaWinget | Out-Null
  $ffmpeg = Get-FfmpegPath
}
if (-not $ffmpeg) {
  Write-Warning 'winget install did not produce ffmpeg.exe. Trying direct download.'
  Install-FfmpegViaDirectDownload | Out-Null
  $ffmpeg = Get-FfmpegPath
}

if (-not $ffmpeg) {
  throw 'ffmpeg.exe was not found after installation.'
}

Write-Output $ffmpeg
