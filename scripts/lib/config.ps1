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
    $RepoRoot = Resolve-Path "$PSScriptRoot\..\.."

    # 1) Explicit environment variable (CI / release builds)
    if ($env:ANNI_CERT_THUMBPRINT) { return $env:ANNI_CERT_THUMBPRINT }

    # 2) Optional user-local thumbprint file
    $thumbFile = "$RepoRoot\certs\thumbprint.txt"
    if (Test-Path $thumbFile) {
        $t = Get-Content $thumbFile -Raw
        if ($t) { return $t.Trim() }
    }

    # 3) Derive from the public certificate
    $cer = "$RepoRoot\certs\AnniAudio.cer"
    if (Test-Path $cer) {
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
        return $cert.Thumbprint
    }

    # 4) Derive from the PFX (password from env)
    $pfx = "$RepoRoot\certs\AnniAudio.pfx"
    if (Test-Path $pfx) {
        $pw = if ($env:ANNI_CERT_PASSWORD) { (New-Object System.Security.SecureString) } else { $null }
        if ($env:ANNI_CERT_PASSWORD) {
            $env:ANNI_CERT_PASSWORD.ToCharArray() | ForEach-Object { $pw.AppendChar($_) }
        }
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($pfx, $pw)
        return $cert.Thumbprint
    }

    throw "Could not determine certificate thumbprint. Set `$env:ANNI_CERT_THUMBPRINT, create certs/thumbprint.txt, or place certs/AnniAudio.cer (or .pfx with `$env:ANNI_CERT_PASSWORD)."
}

# Dot-source this file: . $PSScriptRoot\lib\config.ps1
