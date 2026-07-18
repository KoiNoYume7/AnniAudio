<#
.SYNOPSIS
    Interactive terminal UI for AnniAudio routing.
.DESCRIPTION
    Lists audio endpoints, lets you pick capture/render devices by number,
    set default playback, and start routes with live volume control.
.EXAMPLE
    .\scripts\tui.ps1
#>
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [switch]$Test
)

$ErrorActionPreference = "Stop"

$RouteCli = "$RepoRoot\build\bin\Release\route_cli.exe"

function Test-RouteCli {
    if (!(Test-Path $RouteCli)) {
        Write-Host "ERROR: route_cli.exe not found at $RouteCli" -ForegroundColor Red
        Write-Host "Run '.\cli\anniaudio.ps1 build' first." -ForegroundColor Yellow
        exit 1
    }
}

function Get-Endpoints {
    Test-RouteCli
    $output = & $RouteCli list 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: route_cli list failed" -ForegroundColor Red
        if ($output) { $output | ForEach-Object { Write-Host $_ -ForegroundColor Red } }
        exit 1
    }

    $endpoints = @()
    foreach ($line in $output) {
        # Example: 0    RENDER   [CABLE]      Speakers (My Studio Cable) (default)
        if ($line -match '^\s*(\d+)\s+(RENDER|CAPTURE)\s+\[([^\]]+)\]\s+(.+?)\s*$') {
            $idx      = [int]$matches[1]
            $flow     = $matches[2]
            $type     = $matches[3]
            $nameRaw  = $matches[4].Trim()

            $isDefault = $false
            $name = $nameRaw
            if ($name -match '^(.*?)\s+\(default\)$') {
                $name = $matches[1].Trim()
                $isDefault = $true
            }

            $endpoints += [PSCustomObject]@{
                Index     = $idx
                Flow      = $flow
                Type      = $type
                Name      = $name
                IsDefault = $isDefault
                IsCable   = ($type -eq 'CABLE')
            }
        }
    }
    return $endpoints
}

function Show-EndpointList {
    param([PSCustomObject[]]$Endpoints, [string]$Title)
    Write-Host "`n$Title" -ForegroundColor Cyan
    foreach ($ep in $Endpoints) {
        $def = if ($ep.IsDefault) { " (default)" } else { "" }
        $color = if ($ep.IsCable) { "Green" } else { "White" }
        Write-Host ("  [{0}] {1}{2}" -f $ep.Index, $ep.Name, $def) -ForegroundColor $color
    }
}

function Read-Number {
    param([string]$Prompt, [int]$Min, [int]$Max)
    while ($true) {
        $in = Read-Host $Prompt
        if ($in -eq 'q') { return $null }
        if ($in -match '^\d+$') {
            $n = [int]$in
            if ($n -ge $Min -and $n -le $Max) { return $n }
        }
        Write-Host "Enter a number between $Min and $Max, or q." -ForegroundColor Red
    }
}

function Invoke-Route {
    param([PSCustomObject[]]$Endpoints)
    $captures = $Endpoints | Where-Object { $_.Flow -eq 'CAPTURE' }
    $renders  = $Endpoints | Where-Object { $_.Flow -eq 'RENDER' }

    Show-EndpointList -Endpoints $captures -Title "CAPTURE inputs"
    $cIdx = Read-Number -Prompt "Select CAPTURE # (or q to cancel)" -Min 0 -Max ($captures.Count - 1)
    if ($null -eq $cIdx) { return }
    $capture = $captures | Where-Object { $_.Index -eq $cIdx }

    Show-EndpointList -Endpoints $renders -Title "RENDER outputs"
    $rIdx = Read-Number -Prompt "Select RENDER # (or q to cancel)" -Min 0 -Max ($renders.Count - 1)
    if ($null -eq $rIdx) { return }
    $render = $renders | Where-Object { $_.Index -eq $rIdx }

    $vol = Read-Host "Volume % (1-200, default 100)"
    if ($vol -notmatch '^\d+$') { $vol = "100" }
    [int]$volInt = $vol
    if ($volInt -lt 1)   { $volInt = 1 }
    if ($volInt -gt 200) { $volInt = 200 }

    # If the chosen input is a cable, offer to set default playback to its render side
    # so apps play into it automatically.
    if ($capture.IsCable) {
        # Extract base name, e.g. "My Studio Cable" from "Microphone (My Studio Cable)"
        $base = $null
        if ($capture.Name -match '\(([^)]+)\)$') { $base = $matches[1] }
        $matchingRender = $null
        if ($base) {
            $matchingRender = $renders | Where-Object { $_.IsCable -and $_.Name -like "*$base*" } | Select-Object -First 1
        }

        if ($matchingRender) {
            $setDef = Read-Host "Set default playback to '$($matchingRender.Name)' so apps route into this cable? [y/N]"
            if ($setDef -match '^[yY]') {
                Write-Host "[tui] Setting default playback to: $($matchingRender.Name)" -ForegroundColor Cyan
                & $RouteCli default "$($matchingRender.Name)"
            }
        }
    }

    Write-Host "[tui] Routing: $($capture.Name) -> $($render.Name) at $volInt%" -ForegroundColor Green
    & $RouteCli route "$($capture.Name)" "$($render.Name)" $volInt
}

function Set-DefaultRender {
    param([PSCustomObject[]]$Endpoints)
    $renders = $Endpoints | Where-Object { $_.Flow -eq 'RENDER' }
    Show-EndpointList -Endpoints $renders -Title "Set default playback device"
    $rIdx = Read-Number -Prompt "Select RENDER # (or q to cancel)" -Min 0 -Max ($renders.Count - 1)
    if ($null -eq $rIdx) { return }
    $render = $renders | Where-Object { $_.Index -eq $rIdx }
    Write-Host "[tui] Setting default playback to: $($render.Name)" -ForegroundColor Cyan
    & $RouteCli default "$($render.Name)"
}

function Show-MainMenu {
    Write-Host "`n`n============================================================" -ForegroundColor Cyan
    Write-Host "  AnniAudio Routing TUI" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "  [1] Route audio (choose input & output)"
    Write-Host "  [2] Set default playback device"
    Write-Host "  [3] Restore default to headphones"
    Write-Host "  [4] List endpoints"
    Write-Host "  [q] Quit"
    Write-Host "============================================================" -ForegroundColor Cyan
}

# --- Test mode -------------------------------------------------------------
if ($Test) {
    $endpoints = Get-Endpoints
    $endpoints | Format-Table -AutoSize
    exit 0
}

# --- Main loop -------------------------------------------------------------
while ($true) {
    $endpoints = Get-Endpoints
    Show-MainMenu
    $choice = Read-Host "Choice"

    switch ($choice) {
        '1' { Invoke-Route -Endpoints $endpoints }
        '2' { Set-DefaultRender -Endpoints $endpoints }
        '3' {
            Write-Host "[tui] Restoring default playback to headphones" -ForegroundColor Cyan
            & $RouteCli default "Headphones"
        }
        '4' { & $RouteCli list }
        'q' { exit 0 }
        'Q' { exit 0 }
        default { Write-Host "Invalid choice." -ForegroundColor Red }
    }

    Write-Host "`nPress Enter to continue..." -ForegroundColor DarkGray
    $null = Read-Host
}
