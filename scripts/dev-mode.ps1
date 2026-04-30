<#
.SYNOPSIS
    Switch to dev mode: re-enable AnniAudioCable and turn on test signing.
.NOTES
    Requires admin. Takes effect after reboot.
#>
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host "`n[dev-mode] Re-enabling AnniAudioCable service ..." -ForegroundColor Cyan
sc.exe config AnniAudioCable start= demand 2>$null

Write-Host "[dev-mode] Enabling test signing ..." -ForegroundColor Cyan
bcdedit /set testsigning on

Write-Host @"

[dev-mode] Done.
  - AnniAudioCable will load on next boot.
  - Test signing is ON — do NOT game with anti-cheat while in this mode.

  Reboot now to apply. Run gaming-mode.ps1 before gaming.
"@ -ForegroundColor Green

$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq 'y') { Restart-Computer }
