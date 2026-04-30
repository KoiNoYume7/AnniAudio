<#
.SYNOPSIS
    Switch to gaming mode: disable AnniAudioCable and turn off test signing.
.NOTES
    Requires admin. Takes effect after reboot.
#>
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host "`n[gaming-mode] Disabling AnniAudioCable service ..." -ForegroundColor Cyan
sc.exe config AnniAudioCable start= disabled 2>$null

Write-Host "[gaming-mode] Disabling test signing ..." -ForegroundColor Cyan
bcdedit /set testsigning off

Write-Host @"

[gaming-mode] Done.
  - AnniAudioCable will NOT load on next boot.
  - Test signing is OFF — anti-cheat should be happy.

  Reboot now to apply. Run dev-mode.ps1 when done gaming.
"@ -ForegroundColor Green

$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq 'y') { Restart-Computer }
