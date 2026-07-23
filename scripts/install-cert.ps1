<#
.SYNOPSIS
    Install the AnniAudio test code-signing certificate into the trust stores.
.DESCRIPTION
    Imports certs/AnniAudio.cer into LocalMachine\Root and LocalMachine\TrustedPublisher
    so test-signed AnniAudio drivers can be staged and loaded. Also imports the PFX
    into CurrentUser\My so signtool can access the private key.
.NOTES
    Must be run as Administrator.
#>
$ErrorActionPreference = "Stop"

$RepoRoot = "$PSScriptRoot\.."
$CerFile  = "$RepoRoot\certs\AnniAudio.cer"
$PfxFile  = "$RepoRoot\certs\AnniAudio.pfx"

# Verify admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "install-cert.ps1 requires Administrator privileges."
    exit 1
}

if (!(Test-Path $CerFile)) {
    Write-Error "Certificate not found: $CerFile"
    exit 1
}

# Derive the thumbprint from the certificate file instead of hardcoding it.
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($CerFile)
$Thumbprint = $cert.Thumbprint

Write-Host "[install-cert] Importing AnniAudio certificate ($Thumbprint) into trust stores ..." -ForegroundColor Cyan

# LocalMachine\Root - required so Windows trusts the driver signature chain
$root = Get-ChildItem -Path Cert:\LocalMachine\Root | Where-Object { $_.Thumbprint -eq $Thumbprint }
if (!$root) {
    Import-Certificate -FilePath $CerFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Write-Host "  -> Imported into LocalMachine\Root" -ForegroundColor Green
} else {
    Write-Host "  -> Already present in LocalMachine\Root" -ForegroundColor Gray
}

# LocalMachine\TrustedPublisher - required for driver publisher trust
$pub = Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher | Where-Object { $_.Thumbprint -eq $Thumbprint }
if (!$pub) {
    Import-Certificate -FilePath $CerFile -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Host "  -> Imported into LocalMachine\TrustedPublisher" -ForegroundColor Green
} else {
    Write-Host "  -> Already present in LocalMachine\TrustedPublisher" -ForegroundColor Gray
}

# CurrentUser\My (with private key) for signtool if the PFX is present
if (Test-Path $PfxFile) {
    $my = Get-ChildItem -Path Cert:\CurrentUser\My | Where-Object { $_.Thumbprint -eq $Thumbprint }
    if (!$my) {
        $pw = if ($env:ANNI_CERT_PASSWORD) { ConvertTo-SecureString -String $env:ANNI_CERT_PASSWORD -AsPlainText -Force } else { $null }
        Import-PfxCertificate -FilePath $PfxFile -CertStoreLocation Cert:\CurrentUser\My -Exportable -Password $pw | Out-Null
        Write-Host "  -> Imported PFX into CurrentUser\My" -ForegroundColor Green
    } else {
        Write-Host "  -> PFX already present in CurrentUser\My" -ForegroundColor Gray
    }
}

Write-Host "[install-cert] Done.`n" -ForegroundColor Green
exit 0
