<#
.SYNOPSIS
    Switch to dev mode: enable AnniAudio device and turn on test signing.
.DESCRIPTION
    This script:
      1. Enables any disabled AnniAudio PnP device instances.
      2. Turns on test signing (required for the dev-signed driver).
      3. Restarts the Windows Audio service so the endpoint appears.
    Other virtual cables are NOT touched.
.NOTES
    Requires admin. Takes effect immediately (Audiosrv restart), but test
    signing only takes effect after reboot.
#>
$ErrorActionPreference = "Continue"

Write-Host "`n[dev-mode] Enabling test signing ..." -ForegroundColor Cyan
& bcdedit /set testsigning on
$exit1 = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
if ($exit1 -ne 0) {
    Write-Error "bcdedit failed — cannot enable test signing."
    exit $exit1
}

Write-Host "[dev-mode] Enabling AnniAudio device(s) ..." -ForegroundColor Cyan
$devs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -like "*AnniAudio*"
}
if ($devs) {
    foreach ($dev in $devs) {
        if ($dev.Status -eq "OK") {
            Write-Host "  -> $($dev.FriendlyName) already enabled." -ForegroundColor Green
        } else {
            try {
                Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction Stop
                Write-Host "  -> Enabled $($dev.FriendlyName)" -ForegroundColor Green
            } catch {
                Write-Warning "  Could not enable $($dev.FriendlyName): $_"
            }
        }
    }
} else {
    Write-Warning "No AnniAudio device found in Device Manager. Run install-driver.ps1 first."
}

Write-Host "[dev-mode] Restarting Windows Audio service ..." -ForegroundColor Cyan
Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
Write-Host "  -> Audiosrv restarted." -ForegroundColor Green

Write-Host @"

[dev-mode] Done.
  - AnniAudio device(s) are enabled.
  - Test signing is ON (reboot required for this to take effect).
  - Other virtual cables are unaffected.

  Reboot now so the driver can load. Run gaming-mode.ps1 before gaming.
"@ -ForegroundColor Green

$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq 'y') { Restart-Computer }
