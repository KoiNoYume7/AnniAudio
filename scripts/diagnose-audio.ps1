<#
.SYNOPSIS
    Diagnose virtual audio cable installation status on this machine.
.DESCRIPTION
    Lists all MEDIA-class PnP devices (audio), shows driver details for
    AnniAudio and common VAC products, checks test signing mode, and
    reports Windows Audio service state.
.NOTES
    Must be run as Administrator for full driver store details.
#>
#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "  AnniAudio — Audio Stack Diagnostic Report" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# 1. Test signing mode
# ---------------------------------------------------------------------------
Write-Host "`n[1] Boot Configuration (test signing)" -ForegroundColor Yellow
$bcd = & bcdedit /enum | Select-String "testsigning\s+(\w+)"
if ($bcd) {
    $val = $bcd.Matches[0].Groups[1].Value
    if ($val -eq "Yes") {
        Write-Host "    Test signing: ON  (unsigned drivers will load)" -ForegroundColor Green
    } else {
        Write-Host "    Test signing: OFF (unsigned drivers will NOT load)" -ForegroundColor Red
    }
} else {
    Write-Host "    Could not read test signing status." -ForegroundColor Red
}

# ---------------------------------------------------------------------------
# 2. Windows Audio service
# ---------------------------------------------------------------------------
Write-Host "`n[2] Windows Audio Service (Audiosrv)" -ForegroundColor Yellow
$audiosrv = Get-Service -Name Audiosrv -ErrorAction SilentlyContinue
if ($audiosrv) {
    Write-Host "    Status : $($audiosrv.Status)"
    Write-Host "    Start  : $($audiosrv.StartType)"
} else {
    Write-Host "    NOT FOUND — critical system service missing!" -ForegroundColor Red
}

# ---------------------------------------------------------------------------
# 3. MEDIA-class devices
# ---------------------------------------------------------------------------
Write-Host "`n[3] MEDIA-class PnP Devices (Audio)" -ForegroundColor Yellow
$mediaDevs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Sort-Object FriendlyName
if ($mediaDevs) {
    foreach ($dev in $mediaDevs) {
        $statusColor = switch ($dev.Status) {
            "OK"       { "Green" }
            "Error"    { "Red" }
            "Unknown"  { "Yellow" }
            default    { "White" }
        }
        Write-Host "    [$($dev.Status)] $($dev.FriendlyName)" -ForegroundColor $statusColor
        if ($dev.InstanceId -like "*AnniAudio*") {
            Write-Host "         ^--- AnniAudio device" -ForegroundColor Cyan
        }
    }
} else {
    Write-Host "    No MEDIA-class devices found." -ForegroundColor Red
}

# ---------------------------------------------------------------------------
# 4. Driver packages in store (AnniAudio + common VACs)
# ---------------------------------------------------------------------------
Write-Host "`n[4] Installed Driver Packages (Driver Store)" -ForegroundColor Yellow
$packages = & pnputil /enum-drivers 2>$null
$interesting = @("AnniAudio", "VB-Audio", "Virtual Audio Cable", "VAC", "Voicemeeter", "CABLE")
$inBlock = $false
$currentBlock = @()

foreach ($line in $packages) {
    if ($line -match "Published Name") {
        if ($inBlock) {
            $blockText = $currentBlock -join "`n"
            foreach ($term in $interesting) {
                if ($blockText -like "*$term*") {
                    foreach ($l in $currentBlock) {
                        if ($l.Trim()) {
                            Write-Host "    $l"
                        }
                    }
                    Write-Host "    ---"
                    break
                }
            }
        }
        $inBlock = $true
        $currentBlock = @($line)
    } elseif ($inBlock) {
        $currentBlock += $line
        if ($line -match "^\s*$" -and $currentBlock.Count -gt 4) {
            $inBlock = $false
        }
    }
}

# Flush last block
if ($inBlock -and $currentBlock.Count -gt 0) {
    $blockText = $currentBlock -join "`n"
    foreach ($term in $interesting) {
        if ($blockText -like "*$term*") {
            foreach ($l in $currentBlock) {
                if ($l.Trim()) {
                    Write-Host "    $l"
                }
            }
            Write-Host "    ---"
            break
        }
    }
}

# ---------------------------------------------------------------------------
# 5. Running kernel drivers
# ---------------------------------------------------------------------------
Write-Host "`n[5] Running Kernel Drivers (audio-related)" -ForegroundColor Yellow
$drivers = Get-WindowsDriver -Online -ErrorAction SilentlyContinue | Where-Object {
    $_.ClassName -eq "MEDIA" -or $_.ProviderName -like "*AnniAudio*" -or
    $_.OriginalFileName -like "*AnniAudio*"
}
if ($drivers) {
    $drivers | Select-Object ProviderName, @{N="Driver";E={Split-Path $_.OriginalFileName -Leaf}}, Version | Format-Table -AutoSize | Out-String | Write-Host
} else {
    Write-Host "    (no driver details available via Get-WindowsDriver)"
}

# ---------------------------------------------------------------------------
# 6. Sound endpoints
# ---------------------------------------------------------------------------
Write-Host "`n[6] Active Sound Endpoints (mmdevice API)" -ForegroundColor Yellow
try {
    $endpoints = Get-AudioDevice -List | Select-Object Name, Type, State
    if ($endpoints) {
        $endpoints | Format-Table -AutoSize | Out-String | Write-Host
    } else {
        Write-Host "    (no endpoints returned)"
    }
} catch {
    Write-Host "    Get-AudioDevice not available (install AudioDeviceCmdlets module from PSGallery if needed)." -ForegroundColor Yellow
    # Fallback: list from registry
    $renderers = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\*\Properties" -ErrorAction SilentlyContinue
    $captures  = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\*\Properties" -ErrorAction SilentlyContinue
    $all = @()
    foreach ($r in $renderers) {
        if ($r.'{a45c254e-df1c-4efd-8020-67d146a850e0},2') {
            $all += [PSCustomObject]@{ Name = $r.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'; Type = "Render"; State = "" }
        }
    }
    foreach ($c in $captures) {
        if ($c.'{a45c254e-df1c-4efd-8020-67d146a850e0},2') {
            $all += [PSCustomObject]@{ Name = $c.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'; Type = "Capture"; State = "" }
        }
    }
    if ($all) {
        $all | Format-Table -AutoSize | Out-String | Write-Host
    }
}

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "  End of diagnostic report." -ForegroundColor Cyan
Write-Host "============================================================`n" -ForegroundColor Cyan
