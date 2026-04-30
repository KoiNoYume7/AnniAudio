<#
.SYNOPSIS
    Toggle the AnniAudio virtual cable device on or off without affecting
    test signing or other virtual audio cables.
.DESCRIPTION
    The most common cause of virtual-cable conflicts is the Windows Audio
    service (Audiosrv) getting into a bad state when devices change.
    This script safely disables or enables only the AnniAudio device,
    then restarts Audiosrv so your other VAC recovers cleanly.

    Usage:
      .\toggle-device.ps1 -State Off   # disable AnniAudio, keep other VACs
      .\toggle-device.ps1 -State On    # re-enable AnniAudio
.PARAMETER State
    Desired state: On or Off.
.NOTES
    Must be run as Administrator.
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("On", "Off")]
    [string]$State
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$devs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -like "*AnniAudio*"
}

if (-not $devs) {
    Write-Error "No AnniAudio device found. Run install-driver.ps1 first."
    exit 1
}

foreach ($dev in $devs) {
    if ($State -eq "On") {
        if ($dev.Status -eq "OK") {
            Write-Host "  $($dev.FriendlyName) is already enabled." -ForegroundColor Green
        } else {
            try {
                Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction Stop
                Write-Host "  Enabled $($dev.FriendlyName)" -ForegroundColor Green
            } catch {
                Write-Error "Failed to enable $($dev.FriendlyName): $_"
                exit 1
            }
        }
    } else {
        if ($dev.Status -ne "OK") {
            Write-Host "  $($dev.FriendlyName) is already disabled." -ForegroundColor Green
        } else {
            try {
                Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction Stop
                Write-Host "  Disabled $($dev.FriendlyName)" -ForegroundColor Green
            } catch {
                Write-Error "Failed to disable $($dev.FriendlyName): $_"
                exit 1
            }
        }
    }
}

Write-Host "`n[toggle-device] Restarting Windows Audio service ..." -ForegroundColor Cyan
Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
Write-Host "  -> Audiosrv restarted." -ForegroundColor Green
Write-Host "`n[toggle-device] Done. Your other virtual cables should now work normally.`n" -ForegroundColor Green
