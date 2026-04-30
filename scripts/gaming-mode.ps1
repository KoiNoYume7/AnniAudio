<#
.SYNOPSIS
    Switch to gaming mode: disable AnniAudio device and turn off test signing.
.DESCRIPTION
    This script:
      1. Disables AnniAudio PnP device instances (other virtual cables are untouched).
      2. Turns off test signing (required by many anti-cheat systems).
      3. Restarts the Windows Audio service so the endpoint disappears
         and other virtual cables recover cleanly.
    The AnniAudio driver package stays in the driver store — run dev-mode.ps1
    to re-enable the device later without reinstalling.
.NOTES
    Requires admin. Device change is immediate (Audiosrv restart), but test
    signing only takes effect after reboot.
#>
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

Write-Host "`n[gaming-mode] Disabling AnniAudio device(s) ..." -ForegroundColor Cyan
$devs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -like "*AnniAudio*"
}
if ($devs) {
    foreach ($dev in $devs) {
        if ($dev.Status -ne "OK") {
            Write-Host "  -> $($dev.FriendlyName) already disabled." -ForegroundColor Green
        } else {
            try {
                Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction Stop
                Write-Host "  -> Disabled $($dev.FriendlyName)" -ForegroundColor Green
            } catch {
                Write-Warning "  Could not disable $($dev.FriendlyName): $_"
            }
        }
    }
} else {
    Write-Host "  (no AnniAudio device found — nothing to disable)"
}

Write-Host "[gaming-mode] Disabling test signing ..." -ForegroundColor Cyan
& bcdedit /set testsigning off
if ($LASTEXITCODE -ne 0) {
    Write-Warning "bcdedit failed — test signing may still be ON."
} else {
    Write-Host "  -> Test signing disabled." -ForegroundColor Green
}

Write-Host "[gaming-mode] Restarting Windows Audio service ..." -ForegroundColor Cyan
Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
Write-Host "  -> Audiosrv restarted." -ForegroundColor Green

Write-Host @"

[gaming-mode] Done.
  - AnniAudio device(s) are disabled (other VACs unaffected).
  - Test signing is OFF (reboot required for this to take effect).
  - Audiosrv restarted so remaining devices recover.

  Reboot now to fully disable test signing. Run dev-mode.ps1 when done gaming.
"@ -ForegroundColor Green

$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq 'y') { Restart-Computer }
