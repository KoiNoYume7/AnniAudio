<#
.SYNOPSIS
    Build and test-sign AnniAudioCable.sys.
.DESCRIPTION
    Uses MSBuild + the WDK toolset to compile the kernel driver, then signs
    the .sys and .cat files with the development certificate.
.PARAMETER Config
    Build configuration: Release (default) or Debug.
.PARAMETER Thumbprint
    SHA-1 thumbprint of the signing certificate.
    Defaults to the AnniAudio dev cert created during driver setup.
.EXAMPLE
    .\build-driver.ps1
    .\build-driver.ps1 -Config Debug
#>
param(
    [string]$Config      = "Release",
    [string]$Thumbprint  = "7D2F96B5B17E0E2959C6E20EEF1ED95822572B2F"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot   = "$PSScriptRoot\.."
$DriverDir  = "$RepoRoot\driver"
$ProjFile   = "$DriverDir\AnniAudioCable.vcxproj"
$SignTool   = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
$Inf2Cat   = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe"
$OutDir     = "$RepoRoot\build\driver\$($Config.ToLower())"

# ---- Generate INF from template + config -----------------------------------
Write-Host "`n[build-driver] Generating INF from template ..." -ForegroundColor Cyan
& "$PSScriptRoot\generate-inf.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Error "[build-driver] INF generation failed."
    exit $LASTEXITCODE
}

# ---- Build ----------------------------------------------------------------
Write-Host "`n[build-driver] Building $Config|x64 ..." -ForegroundColor Cyan
$MSBuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $MSBuild $ProjFile /p:Configuration=$Config /p:Platform=x64 /m /nologo `
           /p:SolutionDir="$RepoRoot\\"

if ($LASTEXITCODE -ne 0) {
    Write-Error "[build-driver] MSBuild failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}

# ---- Catalog -------------------------------------------------------------
# inf2cat generates the .cat that PnP requires; output name is lower-case.
Write-Host "`n[build-driver] Generating catalog (inf2cat) ..." -ForegroundColor Cyan
& $Inf2Cat /driver:$OutDir /os:10_X64
if ($LASTEXITCODE -ne 0) {
    Write-Error "[build-driver] inf2cat failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}

# ---- Sign -----------------------------------------------------------------
Write-Host "`n[build-driver] Signing artifacts ..." -ForegroundColor Cyan

$Artifacts = @("$OutDir\AnniAudioCable.sys")
# inf2cat writes a lower-case filename
$CatFile   = Get-ChildItem $OutDir -Filter "*.cat" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if ($CatFile) { $Artifacts += $CatFile }

foreach ($f in $Artifacts) {
    if (!(Test-Path $f)) {
        Write-Warning "[build-driver] Expected artifact not found: $f"
        continue
    }
    Write-Host "  Signing: $f"
    & $SignTool sign /sha1 $Thumbprint /fd sha256 `
                     /tr http://timestamp.digicert.com /td sha256 `
                     $f
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[build-driver] signtool failed on $f"
        exit $LASTEXITCODE
    }
}

Write-Host "`n[build-driver] Done. Artifacts in: $OutDir`n" -ForegroundColor Green
