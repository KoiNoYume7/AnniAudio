<#
.SYNOPSIS
    Uninstall the AnniAudio Virtual Cable driver.
.NOTES
    Must be run as Administrator.
#>
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$DevCon = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"

Write-Host "`n[uninstall-driver] Removing ROOT\AnniAudioCable device node ..." -ForegroundColor Cyan

if (Test-Path $DevCon) {
    & $DevCon remove "ROOT\AnniAudioCable"
} else {
    Write-Warning "devcon.exe not found. Remove the device manually via Device Manager."
}

Write-Host "[uninstall-driver] Removing driver package from store ..." -ForegroundColor Cyan
& pnputil /delete-driver AnniAudioCable.inf /uninstall 2>$null

Write-Host "`n[uninstall-driver] Done.`n" -ForegroundColor Green
