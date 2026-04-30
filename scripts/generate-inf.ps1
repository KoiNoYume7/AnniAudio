<#
.SYNOPSIS
    Generate AnniAudioCable.inf from template + config.
.DESCRIPTION
    Reads config/cables.json and driver/AnniAudioCable.inf.in,
    substitutes placeholders, writes driver/AnniAudioCable.inf.
.NOTES
    Called by build-driver.ps1 before Inf2Cat.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = "$PSScriptRoot\.."
$Template = "$RepoRoot\driver\AnniAudioCable.inf.in"
$OutInf   = "$RepoRoot\driver\AnniAudioCable.inf"
$Config   = "$RepoRoot\config\cables.json"
$Version  = "$RepoRoot\VERSION"

if (!(Test-Path $Template)) {
    Write-Error "Template not found: $Template"
    exit 1
}
if (!(Test-Path $Config)) {
    Write-Error "Config not found: $Config"
    exit 1
}

$cfg = Get-Content $Config -Raw | ConvertFrom-Json
$ver = (Get-Content $Version -Raw).Trim()

# Build driver version number as 1.x.y.z for INF
# Map semver 0.2.0 -> 1.0.2.0 so Windows treats it as a valid driver version
$semver = $ver -split '\.'
$major  = if ($semver[0]) { [int]$semver[0] } else { 0 }
$minor  = if ($semver[1]) { [int]$semver[1] } else { 0 }
$patch  = if ($semver[2]) { [int]$semver[2] } else { 0 }
$driverVer = "1.$major.$minor.$patch"

$now = Get-Date -Format "MM/dd/yyyy"

# For now we support exactly one cable in the config (multi-cable is future work)
$cable = $cfg.cables | Where-Object { $_.enabled } | Select-Object -First 1
if (-not $cable) {
    Write-Warning "No enabled cable found in config; using defaults."
    $cable = @{ name = "AnniAudio Cable 1"; hw_id = "ROOT\AnniAudioCable" }
}

$tpl = Get-Content $Template -Raw

$replacements = @{
    "{{DRIVER_VER_DATE}}"      = $now
    "{{DRIVER_VER_NUMBER}}"    = $driverVer
    "{{HW_ID}}"                = $cable.hw_id
    "{{DEVICE_NAME}}"          = $cable.name
    "{{WAVE_FRIENDLY_NAME}}"   = $cable.endpoint_name
    "{{TOPOLOGY_FRIENDLY_NAME}}" = "$($cable.endpoint_name) Topology"
}

foreach ($kv in $replacements.GetEnumerator()) {
    $tpl = $tpl.Replace($kv.Key, $kv.Value)
}

$tpl | Set-Content $OutInf -Encoding UTF8
Write-Host "[generate-inf] Created $OutInf (version $driverVer, cable: $($cable.name))" -ForegroundColor Green
