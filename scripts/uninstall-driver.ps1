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
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$DevCon = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
$HwId   = "ROOT\AnniAudioCable"

# ---------------------------------------------------------------------------
# 1. Remove device node(s)
# ---------------------------------------------------------------------------
Write-Host "`n[uninstall-driver] Removing device node(s) ..." -ForegroundColor Cyan

$devs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -like "*$HwId*" -or $_.FriendlyName -like "*AnniAudio*"
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

# Also try the raw hardware ID via devcon if available
if (Test-Path $DevCon) {
    & $DevCon remove $HwId 2>$null
}

# ---------------------------------------------------------------------------
# 2. Find and remove the staged driver package(s)
# ---------------------------------------------------------------------------
Write-Host "`n[uninstall-driver] Removing driver package from store ..." -ForegroundColor Cyan

# pnputil -e lists all packages with their original INF name
$oemInf = $null
try {
    $enum = & pnputil /enum-drivers 2>$null
    for ($i = 0; $i -lt $enum.Count; $i++) {
        if ($enum[$i] -match 'Published Name\s+:\s+(oem\d+\.inf)' -and
            ($i + 1 -lt $enum.Count) -and ($enum[$i + 1] -match 'Original Name\s+:\s+AnniAudioCable\.inf')) {
            $oemInf = $matches[1]
            break
        }
    }
} catch { }

if ($oemInf) {
    Write-Host "  -> Found staged package: $oemInf"
    & pnputil /delete-driver $oemInf /uninstall /force 2>$null
    if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 3010) {
        Write-Host "  -> Package removed successfully."
    } else {
        Write-Warning "  pnputil exit code $LASTEXITCODE — package may still be in use."
    }
} else {
    Write-Host "  (no staged AnniAudioCable.inf package found)"
}

# Fallback: try the raw name just in case
& pnputil /delete-driver AnniAudioCable.inf /uninstall /force 2>$null

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
