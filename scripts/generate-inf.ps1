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

# Filter enabled cables; if none, default to one cable so we never emit an empty INF
$enabledCables = $cfg.cables | Where-Object { $_.enabled }
if (-not $enabledCables) {
    Write-Warning "No enabled cables found in config; using default single cable."
    $enabledCables = @(@{ id = 1; name = "AnniAudio Cable 1"; hw_id = "ROOT\AnniAudioCable"; endpoint_name = "AnniAudio Cable 1" })
}

# ---------------------------------------------------------------------------
# Build per-cable INF sections
# ---------------------------------------------------------------------------

# [Standard.NTamd64] lines — one per cable
$manufacturerEntries = @()
# Per-cable interface sections
$interfaceSections = @()
# Per-cable friendly-name strings
$cableStrings = @()

foreach ($cable in $enabledCables) {
    $idx   = $cable.id
    $name  = $cable.name
    $hwId  = $cable.hw_id
    $epName = if ($cable.endpoint_name) { $cable.endpoint_name } else { $name }

    $safeIdx = $idx -replace '[^0-9]', ''
    $varName = "Cable${safeIdx}Name"

    # Manufacturer entry
    $manufacturerEntries += "%$varName% = AnniAudioCable_Device, $hwId"

    # Interface section for this cable (unique AddInterface set)
    $waveFriendlyVar    = "Wave${safeIdx}.FriendlyName"
    $topoFriendlyVar   = "Topology${safeIdx}.FriendlyName"

    $interfaceSections += @"

; --- Cable $idx ($name) ---
[AnniAudioCable_Device.NT.Interfaces]
AddInterface=%KSCATEGORY_AUDIO%,%KSNAME_Wave%,AnniAudioCable.I.Wave${safeIdx}
AddInterface=%KSCATEGORY_RENDER%,%KSNAME_Wave%,AnniAudioCable.I.Wave${safeIdx}
AddInterface=%KSCATEGORY_CAPTURE%,%KSNAME_Wave%,AnniAudioCable.I.Wave${safeIdx}
AddInterface=%KSCATEGORY_TOPOLOGY%,%KSNAME_Topology%,AnniAudioCable.I.Topology${safeIdx}
AddInterface=%KSCATEGORY_REALTIME%,%KSNAME_Wave%,AnniAudioCable.I.Wave${safeIdx}

[AnniAudioCable.I.Wave${safeIdx}]
AddReg=AnniAudioCable.I.Wave${safeIdx}.AddReg

[AnniAudioCable.I.Wave${safeIdx}.AddReg]
HKR,,CLSID,,%Proxy.CLSID%
HKR,,FriendlyName,,%$waveFriendlyVar%

[AnniAudioCable.I.Topology${safeIdx}]
AddReg=AnniAudioCable.I.Topology${safeIdx}.AddReg

[AnniAudioCable.I.Topology${safeIdx}.AddReg]
HKR,,CLSID,,%Proxy.CLSID%
HKR,,FriendlyName,,%$topoFriendlyVar%
"@

    # String definitions for this cable
    $cableStrings += "$varName         = `"$name`""
    $cableStrings += "$waveFriendlyVar  = `"$epName`""
    $cableStrings += "$topoFriendlyVar  = `"${epName} Topology`""
}

# ---------------------------------------------------------------------------
# Assemble replacements
# ---------------------------------------------------------------------------
$tpl = Get-Content $Template -Raw

$replacements = @{
    "{{DRIVER_VER_DATE}}"           = $now
    "{{DRIVER_VER_NUMBER}}"         = $driverVer
    "{{MANUFACTURER_ENTRIES}}"      = ($manufacturerEntries -join "`n")
    "{{CABLE_INTERFACE_SECTIONS}}"  = ($interfaceSections -join "`n")
    "{{CABLE_STRINGS}}"             = ($cableStrings -join "`n")
}

foreach ($kv in $replacements.GetEnumerator()) {
    $tpl = $tpl.Replace($kv.Key, $kv.Value)
}

$tpl | Set-Content $OutInf -Encoding UTF8
$cableCount = $enabledCables.Count
Write-Host "[generate-inf] Created $OutInf (version $driverVer, $cableCount cable(s))" -ForegroundColor Green
