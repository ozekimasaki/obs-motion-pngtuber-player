param(
    [Parameter(Mandatory = $true)][string]$ObsDll,
    [Parameter(Mandatory = $true)][string]$Dumpbin,
    [Parameter(Mandatory = $true)][string]$LibExe,
    [Parameter(Mandatory = $true)][string]$OutDef,
    [Parameter(Mandatory = $true)][string]$OutLib
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ObsDll)) {
    throw "obs.dll not found: $ObsDll"
}

$defDir = Split-Path -Parent $OutDef
$libDir = Split-Path -Parent $OutLib
if ($defDir) {
    New-Item -ItemType Directory -Force -Path $defDir | Out-Null
}
if ($libDir) {
    New-Item -ItemType Directory -Force -Path $libDir | Out-Null
}

$dump = & $Dumpbin /exports $ObsDll
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $ObsDll"
}

$exports = New-Object System.Collections.Generic.List[string]
foreach ($line in $dump) {
    if ($line -match '^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+([^\s=]+)') {
        $name = $Matches[1]
        if ($name -and -not $exports.Contains($name)) {
            $exports.Add($name)
        }
    }
}

if ($exports.Count -eq 0) {
    throw 'No exports were parsed from obs.dll'
}

$defLines = @('LIBRARY obs.dll', 'EXPORTS')
$defLines += $exports | ForEach-Object { "  $_" }
Set-Content -Path $OutDef -Value $defLines -Encoding Ascii

& $LibExe "/def:$OutDef" '/machine:x64' "/out:$OutLib" | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "lib.exe failed while generating $OutLib"
}
