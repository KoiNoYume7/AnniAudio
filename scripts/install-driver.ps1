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
$ErrorActionPreference = "Stop"

$RepoRoot  = "$PSScriptRoot\.."
$InfPath   = "$RepoRoot\build\driver\release\AnniAudioCable.inf"
$DevCon    = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
$Config    = "$RepoRoot\config\cables.json"

# Read cable configuration
if (Test-Path $Config) {
    $cfg = Get-Content $Config -Raw | ConvertFrom-Json
    $enabledCables = $cfg.cables | Where-Object { $_.enabled }
} else {
    $enabledCables = @(@{ hw_id = "ROOT\AnniAudioCable"; name = "AnniAudio Cable 1" })
}

if (-not $enabledCables) {
    Write-Error "No enabled cables found in config. Run 'anniaudio.ps1 config init' first."
    exit 1
}

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

# ---------------------------------------------------------------------------
# 0b. Detect existing installation (device or staged package)
# ---------------------------------------------------------------------------
$hwIdPatterns = $enabledCables | ForEach-Object { "*$($_.hw_id)*" }
$existingDevs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $dev = $_
    ($hwIdPatterns | Where-Object { $dev.InstanceId -like $_ }) -or
    ($dev.FriendlyName -like "*AnniAudio*")
}

# Check driver store for any staged AnniAudio package
$existingPkg = $null
try {
    $enum = & pnputil /enum-drivers 2>$null
    for ($i = 0; $i -lt $enum.Count; $i++) {
        if ($enum[$i] -match 'Published Name\s+:\s+(oem\d+\.inf)' -and
            ($i + 1 -lt $enum.Count) -and ($enum[$i + 1] -match 'Original Name\s+:\s+AnniAudioCable\.inf')) {
            $existingPkg = $matches[1]
            break
        }
    }
} catch { }

if ($existingDevs -or $existingPkg) {
    Write-Warning "AnniAudio appears to already be installed:"
    if ($existingDevs) {
        $existingDevs | ForEach-Object { Write-Warning "  Device : $($_.FriendlyName) [$($_.InstanceId)]" }
    }
    if ($existingPkg) {
        Write-Warning "  Package: $existingPkg (staged in driver store)"
    }
    $reinstall = Read-Host "Re-install (uninstall first, then clean install)? [y/N]"
    if ($reinstall -eq 'y') {
        Write-Host "`n[install-driver] Cleaning up existing installation ..." -ForegroundColor Cyan
        # Remove device nodes
        foreach ($dev in $existingDevs) {
            try {
                & pnputil /remove-device $dev.InstanceId 2>$null
                Write-Host "  -> Removed device $($dev.FriendlyName)"
            } catch {
                Write-Warning "  Failed to remove device $($dev.InstanceId)"
            }
        }
        if (Test-Path $DevCon) {
            foreach ($cable in $enabledCables) {
                & $DevCon remove $cable.hw_id 2>$null
            }
        }
        # Remove staged package
        if ($existingPkg) {
            & pnputil /delete-driver $existingPkg /uninstall /force 2>$null
            Write-Host "  -> Removed staged package $existingPkg"
        }
        & pnputil /delete-driver "AnniAudioCable.inf" /uninstall /force 2>$null
    } else {
        Write-Host "Skipping install — existing installation kept." -ForegroundColor Yellow
        exit 0
    }
}

# ---------------------------------------------------------------------------
# 1. Stage driver package
# ---------------------------------------------------------------------------
Write-Host "`n[install-driver] Staging driver package ..." -ForegroundColor Cyan
& pnputil /add-driver $InfPath /install
$addExit = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
if ($addExit -ne 0) {
    # 259 = package already exists with same name; try force-update
    if ($addExit -eq 259 -or $addExit -eq -2147024891) {
        Write-Warning "Package already staged; attempting forced update ..."
        & pnputil /add-driver $InfPath /install /force
        $forceExit = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
        if ($forceExit -ne 0) {
            Write-Error "pnputil force-update failed (exit $forceExit)."
            exit $forceExit
        }
    } else {
        Write-Error "pnputil failed (exit $addExit)."
        exit $addExit
    }
}
Write-Host "  -> Driver staged successfully." -ForegroundColor Green

# ---------------------------------------------------------------------------
# 2. Create device nodes for all enabled cables
# ---------------------------------------------------------------------------
if (Test-Path $DevCon) {
    foreach ($cable in $enabledCables) {
        Write-Host "`n[install-driver] Creating device node $($cable.hw_id) ($($cable.name)) ..." -ForegroundColor Cyan
        & $DevCon install $InfPath $cable.hw_id
        # devcon may return 0 (success) or 1 (device already exists); both are fine
        $dcExit = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
        if ($dcExit -ne 0 -and $dcExit -ne 1) {
            Write-Warning "devcon returned $dcExit for $($cable.hw_id) — device node may not have been created."
        } else {
            Write-Host "  -> Device node created." -ForegroundColor Green
        }
    }
} else {
    Write-Warning "devcon.exe not found at:`n  $DevCon`nCreate devices manually via Device Manager > Add legacy hardware."
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
