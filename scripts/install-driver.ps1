<#
.SYNOPSIS
    Install the AnniAudio Virtual Cable driver on this machine.
.DESCRIPTION
    1. Adds the signed driver package to the Windows driver store.
    2. Creates the ROOT\AnniAudioCable device node via devcon.exe so the
       driver is instantiated without real hardware.
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

if (!(Test-Path $InfPath)) {
    Write-Error "INF not found at $InfPath — run build-driver.ps1 first."
    exit 1
}

Write-Host "`n[install-driver] Adding driver package to store ..." -ForegroundColor Cyan
& pnputil /add-driver $InfPath /install
if ($LASTEXITCODE -ne 0) { Write-Error "pnputil failed."; exit $LASTEXITCODE }

if (Test-Path $DevCon) {
    Write-Host "`n[install-driver] Creating device node ROOT\AnniAudioCable ..." -ForegroundColor Cyan
    & $DevCon install $InfPath "ROOT\AnniAudioCable"
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1) {
        Write-Error "devcon install failed (exit $LASTEXITCODE)."
        exit $LASTEXITCODE
    }
} else {
    Write-Warning "devcon.exe not found at expected path:`n  $DevCon`nCreate the device node manually via Device Manager > Add legacy hardware."
}

Write-Host "`n[install-driver] Driver installed successfully.`n" -ForegroundColor Green
