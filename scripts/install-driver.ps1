<#
.SYNOPSIS
    Install the AnniAudio Virtual Cable driver on this machine.
.DESCRIPTION
    1. Verifies test signing is ON (required for dev-signed drivers).
    2. Stages the signed driver package into the Windows driver store.
    3. Creates the ROOT\AnniAudioCable device node via devcon.exe.
    4. Restarts the Windows Audio service so the new endpoint appears
       and other virtual cables recover cleanly.
.NOTES
    Must be run as Administrator.
    Requires devcon.exe from the WDK Tools folder.
#>
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot  = "$PSScriptRoot\.."
$InfPath   = "$RepoRoot\build\driver\release\AnniAudioCable.inf"
$DevCon    = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
$HwId      = "ROOT\AnniAudioCable"

# ---------------------------------------------------------------------------
# 0. Pre-flight checks
# ---------------------------------------------------------------------------

# Test signing must be ON for an unsigned/test-signed driver to load
$bcd = & bcdedit /enum | Select-String "testsigning\s+(\w+)"
$tsOn = $false
if ($bcd) { $tsOn = ($bcd.Matches[0].Groups[1].Value -eq "Yes") }

if (-not $tsOn) {
    Write-Error @"
Test signing is OFF. The AnniAudio driver is test-signed and will NOT load.
Enable test signing first, then reboot:
    bcdedit /set testsigning on
Then reboot and run this script again.
"@
    exit 1
}

if (!(Test-Path $InfPath)) {
    Write-Error "INF not found at $InfPath — run build-driver.ps1 first."
    exit 1
}

# Detect existing AnniAudio device to avoid duplicates
$existing = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -like "*$HwId*" -or $_.FriendlyName -like "*AnniAudio*"
}
if ($existing) {
    Write-Warning "AnniAudio device(s) already present:"
    $existing | ForEach-Object { Write-Warning "  $($_.FriendlyName) [$($_.InstanceId)]" }
    $cont = Read-Host "Continue anyway? [y/N]"
    if ($cont -ne 'y') { exit 0 }
}

# ---------------------------------------------------------------------------
# 1. Stage driver package
# ---------------------------------------------------------------------------
Write-Host "`n[install-driver] Staging driver package ..." -ForegroundColor Cyan
& pnputil /add-driver $InfPath /install
if ($LASTEXITCODE -ne 0) {
    Write-Error "pnputil failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}
Write-Host "  -> Driver staged successfully." -ForegroundColor Green

# ---------------------------------------------------------------------------
# 2. Create device node
# ---------------------------------------------------------------------------
if (Test-Path $DevCon) {
    Write-Host "`n[install-driver] Creating device node $HwId ..." -ForegroundColor Cyan
    & $DevCon install $InfPath $HwId
    # devcon may return 0 (success) or 1 (device already exists); both are fine
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1) {
        Write-Warning "devcon returned $LASTEXITCODE — device node may not have been created."
    } else {
        Write-Host "  -> Device node created." -ForegroundColor Green
    }
} else {
    Write-Warning "devcon.exe not found at:`n  $DevCon`nCreate the device manually via Device Manager > Add legacy hardware."
}

# ---------------------------------------------------------------------------
# 3. Restart audio stack
# ---------------------------------------------------------------------------
Write-Host "`n[install-driver] Restarting Windows Audio service ..." -ForegroundColor Cyan
Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
Write-Host "  -> Audiosrv restarted." -ForegroundColor Green

# ---------------------------------------------------------------------------
# 4. Verify
# ---------------------------------------------------------------------------
Start-Sleep -Seconds 2
$newDev = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -like "*AnniAudio*"
}
if ($newDev) {
    Write-Host "`n[install-driver] Detected endpoint(s):" -ForegroundColor Green
    $newDev | ForEach-Object { Write-Host "  OK  $($_.FriendlyName) [$($_.Status)]" -ForegroundColor Green }
} else {
    Write-Warning "AnniAudio endpoint not yet visible. It may appear after a few seconds or require a reboot."
}

Write-Host "`n[install-driver] Done.`n" -ForegroundColor Green
