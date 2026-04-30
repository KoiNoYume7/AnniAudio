<#
.SYNOPSIS
    AnniAudio Control Panel CLI
.DESCRIPTION
    Unified command-line interface for managing the AnniAudio Virtual Audio
    Cable driver, installation, configuration, and test-signing mode.

    Subcommands:
      install       Build driver from source, generate INF, stage & install.
      uninstall     Completely remove driver and clean up driver store.
      dev-mode      Enable test signing and activate AnniAudio device.
      gaming-mode   Disable test signing and deactivate AnniAudio device.
      status        Show diagnostic report (devices, driver store, signing).
      config        Manage cable configuration (get, set, init).
      build         Build driver without installing.

    Usage:
      .\cli\anniaudio.ps1 <subcommand> [options]

    Examples:
      .\cli\anniaudio.ps1 install
      .\cli\anniaudio.ps1 config set cables[0].name "My Cable"
      .\cli\anniaudio.ps1 status
      .\cli\anniaudio.ps1 gaming-mode
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("install", "uninstall", "dev-mode", "gaming-mode", "status", "config", "build", "version")]
    [string]$Command,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
$CLI_DIR  = $PSScriptRoot
$REPO_ROOT = Resolve-Path "$CLI_DIR\.."
$SCRIPTS   = "$REPO_ROOT\scripts"

. "$SCRIPTS\lib\config.ps1"

$Version = Get-AnniVersion

function Show-Header {
    Write-Host "`n============================================================" -ForegroundColor Cyan
    Write-Host "  AnniAudio Control Panel  v$Version" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
}

function Invoke-Script {
    param([string]$Name, [string[]]$ScriptArgs = @())
    $path = "$SCRIPTS\$Name"
    if (!(Test-Path $path)) {
        Write-Error "Script not found: $path"
        exit 1
    }
    & $path @ScriptArgs
    return $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# install
# ---------------------------------------------------------------------------
function Invoke-AnniInstall {
    Show-Header
    Write-Host "[install] Building driver ..." -ForegroundColor Cyan

    # 1. Generate INF from template + config
    $genExit = Invoke-Script "generate-inf.ps1"
    if ($genExit -ne 0) { exit $genExit }

    # 2. Build driver
    $buildExit = Invoke-Script "build-driver.ps1"
    if ($buildExit -ne 0) { exit $buildExit }

    # 3. Install driver
    $installExit = Invoke-Script "install-driver.ps1"
    if ($installExit -ne 0) { exit $installExit }

    Write-Host "`n[install] Complete. AnniAudio is ready." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# uninstall
# ---------------------------------------------------------------------------
function Invoke-AnniUninstall {
    Show-Header
    Write-Host "[uninstall] Removing AnniAudio driver ..." -ForegroundColor Cyan
    $exit = Invoke-Script "uninstall-driver.ps1"
    if ($exit -ne 0) { exit $exit }
    Write-Host "`n[uninstall] Complete." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# dev-mode / gaming-mode
# ---------------------------------------------------------------------------
function Invoke-AnniDevMode {
    Show-Header
    Invoke-Script "dev-mode.ps1"
}

function Invoke-AnniGamingMode {
    Show-Header
    Invoke-Script "gaming-mode.ps1"
}

# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------
function Invoke-AnniStatus {
    Show-Header
    Invoke-Script "diagnose-audio.ps1"
}

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
function Invoke-AnniBuild {
    Show-Header
    Write-Host "[build] Generating INF ..." -ForegroundColor Cyan
    $genExit = Invoke-Script "generate-inf.ps1"
    if ($genExit -ne 0) { exit $genExit }

    Write-Host "[build] Building driver ..." -ForegroundColor Cyan
    $buildExit = Invoke-Script "build-driver.ps1"
    if ($buildExit -ne 0) { exit $buildExit }

    Write-Host "`n[build] Done. Artifacts in build\driver\release." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# config
# ---------------------------------------------------------------------------
function Invoke-AnniConfig {
    param([string[]]$CfgArgs)

    Show-Header
    $action = if ($CfgArgs.Count -gt 0) { $CfgArgs[0] } else { "" }

    switch ($action.ToLower()) {
        "init" {
            $dest = "$REPO_ROOT\config\cables.json"
            if (Test-Path $dest) {
                $overwrite = Read-Host "config/cables.json already exists. Overwrite? [y/N]"
                if ($overwrite -ne 'y') { return }
            }
            $default = @{
                schema_version = 1
                cables = @(@{
                    id = 1
                    name = "AnniAudio Cable 1"
                    enabled = $true
                    hw_id = "ROOT\AnniAudioCable"
                    endpoint_name = "AnniAudio Cable 1"
                })
                settings = @{
                    default_sample_rate = 48000
                    default_bit_depth = 32
                    default_channels = 2
                    allow_96k = $true
                }
            }
            $default | ConvertTo-Json -Depth 5 | Set-Content $dest -Encoding UTF8
            Write-Host "Created default config at $dest" -ForegroundColor Green
        }

        "get" {
            $cfg = Read-AnniConfig
            if ($CfgArgs.Count -gt 1) {
                $path = $CfgArgs[1]
                $obj = $cfg
                foreach ($seg in $path -split '\.') {
                    if ($seg -match '^(.+)\[(\d+)\]$') {
                        $arrName = $matches[1]
                        $idx = [int]$matches[2]
                        $obj = $obj.$arrName[$idx]
                    } else {
                        $obj = $obj.$seg
                    }
                }
                Write-Output ($obj | ConvertTo-Json -Depth 5)
            } else {
                Write-Output ($cfg | ConvertTo-Json -Depth 5)
            }
        }

        "set" {
            if ($CfgArgs.Count -lt 2) {
                Write-Error "Usage: config set <path> <value>`n  Example: config set cables[0].name \"My Cable\""
                exit 1
            }
            $path = $CfgArgs[1]
            $value = $CfgArgs[2]
            $cfg = Read-AnniConfig

            $segments = $path -split '\.'
            $obj = $cfg
            for ($i = 0; $i -lt $segments.Count - 1; $i++) {
                $seg = $segments[$i]
                if ($seg -match '^(.+)\[(\d+)\]$') {
                    $arrName = $matches[1]
                    $idx = [int]$matches[2]
                    $obj = $obj.$arrName[$idx]
                } else {
                    $obj = $obj.$seg
                }
            }

            $last = $segments[-1]
            # Attempt numeric/bool conversion
            $typedValue = $value
            if ($value -eq "true")  { $typedValue = $true }
            elseif ($value -eq "false") { $typedValue = $false }
            elseif ($value -match '^\d+$') { $typedValue = [int]$value }

            $obj.$last = $typedValue
            Write-AnniConfig $cfg
            Write-Host "Set $path = $typedValue" -ForegroundColor Green
        }

        "add-cable" {
            $name = if ($CfgArgs.Count -gt 1) { $CfgArgs[1] } else { "AnniAudio Cable" }
            $cfg = Read-AnniConfig

            $maxId = 0
            foreach ($c in $cfg.cables) {
                if ($c.id -gt $maxId) { $maxId = $c.id }
            }
            $newId = $maxId + 1
            $hwId = "ROOT\AnniAudioCable$newId"
            $epName = if ($CfgArgs.Count -gt 2) { $CfgArgs[2] } else { "$name $newId" }

            $newCable = [ordered]@{
                id            = $newId
                name          = "$name $newId"
                enabled       = $true
                hw_id         = $hwId
                endpoint_name = $epName
            }
            $cfg.cables += $newCable
            Write-AnniConfig $cfg
            Write-Host "Added cable #$newId : $($newCable.name) (hw_id=$hwId)" -ForegroundColor Green
            Write-Host "Run 'build' then 'install' to create the new device node." -ForegroundColor Yellow
        }

        "remove-cable" {
            if ($CfgArgs.Count -lt 2) {
                Write-Error "Usage: config remove-cable <index>`n  Example: config remove-cable 1"
                exit 1
            }
            $idx = [int]$CfgArgs[1]
            $cfg = Read-AnniConfig
            $found = $cfg.cables | Where-Object { $_.id -eq $idx }
            if (-not $found) {
                Write-Error "Cable with id=$idx not found."
                exit 1
            }
            $cfg.cables = $cfg.cables | Where-Object { $_.id -ne $idx }
            Write-AnniConfig $cfg
            Write-Host "Removed cable #$idx." -ForegroundColor Green
            Write-Host "Run 'uninstall' then 'install' to apply the change." -ForegroundColor Yellow
        }

        "list-cables" {
            $cfg = Read-AnniConfig
            Write-Host "`nConfigured cables:" -ForegroundColor Cyan
            foreach ($c in $cfg.cables) {
                $status = if ($c.enabled) { "ON " } else { "OFF" }
                Write-Host "  [$status] #$($c.id) : $($c.name)  (hw_id=$($c.hw_id), ep=$($c.endpoint_name))" -ForegroundColor $(if ($c.enabled) { "Green" } else { "Gray" })
            }
            Write-Host ""
        }

        default {
            Write-Host @"
Usage: config <action> [args]

Actions:
  init                    Create default config/cables.json
  get [path]              Read config (optionally a dotted path)
  set <path> <val>        Set a value by dotted path
  add-cable [name]        Add a new cable (default name: AnniAudio Cable)
  remove-cable <id>       Remove cable by id
  list-cables             Show all configured cables

Examples:
  config get
  config get cables[0].name
  config set cables[0].name "Studio Cable"
  config set cables[0].enabled false
  config add-cable "My Cable"
  config remove-cable 2
"@ -ForegroundColor Yellow
        }
    }
}

# ---------------------------------------------------------------------------
# version
# ---------------------------------------------------------------------------
function Invoke-AnniVersion {
    Show-Header
    Write-Host "AnniAudio version: $Version" -ForegroundColor Green
    $git = & git -C $REPO_ROOT describe --tags --always 2>$null
    if ($git) { Write-Host "Git ref: $git" -ForegroundColor Green }
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------
switch ($Command) {
    "install"      { Invoke-AnniInstall }
    "uninstall"    { Invoke-AnniUninstall }
    "dev-mode"     { Invoke-AnniDevMode }
    "gaming-mode"  { Invoke-AnniGamingMode }
    "status"       { Invoke-AnniStatus }
    "build"        { Invoke-AnniBuild }
    "config"       { Invoke-AnniConfig -CfgArgs $Args }
    "version"      { Invoke-AnniVersion }
    default        { Write-Host "Unknown command: $Command" -ForegroundColor Red }
}
