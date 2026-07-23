<#
.SYNOPSIS
    Completely uninstall the AnniAudio Virtual Cable driver.
.DESCRIPTION
    1. Removes all device instances (ROOT\AnniAudioCable).
    2. Removes the staged driver package from the driver store.
    3. Restarts Windows Audio service so other virtual cables recover.
.NOTES
    Must be run as Administrator.
#>
$ErrorActionPreference = "Stop"

$RepoRoot = "$PSScriptRoot\.."
$Config   = "$RepoRoot\config\cables.json"

. "$PSScriptRoot\lib\config.ps1"
$DevCon = Get-DevConPath

# Read configured cable HW IDs and endpoint names
if (Test-Path $Config) {
    $cfg = Get-Content $Config -Raw | ConvertFrom-Json
    $enabledCables = $cfg.cables | Where-Object { $_.enabled }
} else {
    $enabledCables = @(@{ hw_id = "ROOT\AnniAudioCable"; name = "AnniAudio Cable 1"; endpoint_name = "AnniAudio Cable 1" })
}
$hwIds      = $enabledCables | ForEach-Object { $_.hw_id }
$hwIdLike   = $hwIds | ForEach-Object { "*$_*" }
$namesLike  = $enabledCables | ForEach-Object { "*$($_.name)*"; "*$($_.endpoint_name)*" } | Select-Object -Unique

# ---------------------------------------------------------------------------
# 1. Remove device node(s)
# ---------------------------------------------------------------------------
Write-Host "`n[uninstall-driver] Removing device node(s) ..." -ForegroundColor Cyan

# Locate by hardware ID property or by friendly name from config
$devs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $dev = $_
    ($dev.FriendlyName -and (($namesLike | Where-Object { $dev.FriendlyName -like $_ }) -ne $null)) -or
    ($hwIdLike | Where-Object { $dev.InstanceId -like $_ }) -or
    ((Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data | Where-Object { $id = $_; $hwIds | Where-Object { $id -eq $_ } }) -ne $null
}

if ($devs) {
    foreach ($dev in $devs) {
        Write-Host "  -> Removing $($dev.FriendlyName) [$($dev.InstanceId)]"
        try {
            & pnputil /remove-device $dev.InstanceId 2>$null
        } catch {
            Write-Warning "  pnputil remove failed for $($dev.InstanceId). Trying devcon..."
            if (Test-Path $DevCon) {
                & $DevCon remove "@$($dev.InstanceId)"
            }
        }
    }
} else {
    Write-Host "  (no AnniAudio device nodes found)"
}

# Also try the raw hardware IDs via devcon if available
if (Test-Path $DevCon) {
    foreach ($hwId in $hwIds) {
        & $DevCon remove $hwId 2>$null
    }
}

# ---------------------------------------------------------------------------
# 2. Find and remove the staged driver package(s)
# ---------------------------------------------------------------------------
Write-Host "`n[uninstall-driver] Removing driver package from store ..." -ForegroundColor Cyan

$oemInf = $null
try {
    $enum = & pnputil /enum-drivers 2>$null
    $candidate = $null
    for ($i = 0; $i -lt $enum.Count; $i++) {
        if ($enum[$i] -match 'Published Name\s*:\s*(oem\d+\.inf)') {
            $candidate = $matches[1]
        }
        if ($candidate -and ($i -lt $enum.Count) -and ($enum[$i] -match 'Original Name\s*:\s*AnniAudioCable\.inf')) {
            $oemInf = $candidate
            break
        }
    }
} catch { }

if ($oemInf) {
    Write-Host "  -> Found staged package: $oemInf"
    & pnputil /delete-driver $oemInf /force 2>$null
    $delExit = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
    if ($delExit -eq 0 -or $delExit -eq 3010) {
        Write-Host "  -> Package removed successfully."
    } else {
        Write-Warning "  pnputil exit code $delExit — package may still be in use."
    }
} else {
    Write-Host "  (no staged AnniAudioCable.inf package found)"
}

# ---------------------------------------------------------------------------
# 3. Restart audio stack so other virtual cables recover
# ---------------------------------------------------------------------------
Write-Host "`n[uninstall-driver] Restarting Windows Audio service ..." -ForegroundColor Cyan
Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
Write-Host "  -> Audiosrv restarted."

# ---------------------------------------------------------------------------
# 4. Check if test signing is still on (warn user)
# ---------------------------------------------------------------------------
$ts = & bcdedit /enum | Select-String "testsigning\s+(\w+)"
if ($ts -and ($ts.Matches[0].Groups[1].Value -eq "Yes")) {
    Write-Host "`n  NOTE: Test signing is still ON." -ForegroundColor Yellow
    Write-Host "        If your other virtual cable requires normal signing, run:"
    Write-Host "          bcdedit /set testsigning off"
    Write-Host "        Then reboot."
}

Write-Host "`n[uninstall-driver] Done.`n" -ForegroundColor Green
