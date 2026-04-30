<#
.SYNOPSIS
    Config helpers for AnniAudio CLI.
.DESCRIPTION
    Reads and writes config/cables.json, resolves paths, validates schema.
#>
$ErrorActionPreference = "Stop"

function Get-AnniConfigPath {
    return Resolve-Path "$PSScriptRoot\..\..\config\cables.json"
}

function Read-AnniConfig {
    $path = Get-AnniConfigPath
    if (-not (Test-Path $path)) {
        throw "Config not found at $path. Run 'anniaudio.ps1 config init' first."
    }
    return Get-Content $path -Raw | ConvertFrom-Json
}

function Write-AnniConfig ([Parameter(Mandatory)] $Config) {
    $path = Get-AnniConfigPath
    $Config | ConvertTo-Json -Depth 5 | Set-Content $path -Encoding UTF8
}

function Get-AnniVersion {
    $vFile = Resolve-Path "$PSScriptRoot\..\..\VERSION"
    if (Test-Path $vFile) { return (Get-Content $vFile -Raw).Trim() }
    return "0.0.0"
}

function Get-AnniBuildDir {
    param([string]$Config = "Release")
    return Resolve-Path "$PSScriptRoot\..\..\build\driver\$($Config.ToLower())"
}

function Get-DevConPath {
    $default = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
    if (Test-Path $default) { return $default }
    # Try other WDK versions
    $candidates = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Tools\*\x64\devcon.exe" -ErrorAction SilentlyContinue | Sort-Object -Descending | Select-Object -First 1
    if ($candidates) { return $candidates.FullName }
    return $default
}

function Get-CertificateThumbprint {
    $thumbFile = Resolve-Path "$PSScriptRoot\..\..\certs\thumbprint.txt" -ErrorAction SilentlyContinue
    if ($thumbFile) {
        $t = Get-Content $thumbFile -Raw
        if ($t) { return $t.Trim() }
    }
    # Fallback to hardcoded known thumbprint (keep in sync with build-driver.ps1)
    return "7D2F96B5B17E0E2959C6E20EEF1ED95822572B2F"
}

# Dot-source this file: . $PSScriptRoot\lib\config.ps1
