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

$ErrorActionPreference = "Stop"

Import-Module "$PSScriptRoot\lib\AnniLog.psd1" -Force

$RepoRoot   = "$PSScriptRoot\.."
$DriverDir  = "$RepoRoot\driver"
$ProjFile   = "$DriverDir\AnniAudioCable.vcxproj"
$SignTool   = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
$Inf2Cat   = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x86\Inf2Cat.exe"
$OutDir     = "$RepoRoot\build\driver\$($Config.ToLower())"
$LogDir     = "$RepoRoot\build\logs"

Initialize-AnniLog -LogFilePath "$LogDir\build-driver.log" -LogLevel "INFO" -EnableStopwatch

# ---- Generate INF from template + config -----------------------------------
Write-AnniLog -Level INFO -Message "Generating INF from template ..."
$LASTEXITCODE = 0
& "$PSScriptRoot\generate-inf.ps1"
$genExit = $LASTEXITCODE
if ($genExit -ne 0) {
    Write-AnniLog -Level ERROR -Message "INF generation failed (exit $genExit)."
    Close-AnniLog
    exit $genExit
}

# ---- Find MSBuild ---------------------------------------------------------
$MSBuild = $null
$VSWHERE = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $VSWHERE) {
    # Try to find MSBuild via vswhere (most reliable)
    $MSBuild = & $VSWHERE -latest -requires Microsoft.Component.MSBuild -find MSBuild\Current\Bin\amd64\MSBuild.exe 2>$null | Select-Object -First 1
}

if (!$MSBuild -or !(Test-Path $MSBuild)) {
    # Fallback: common locations
    $Fallbacks = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )
    foreach ($path in $Fallbacks) {
        if (Test-Path $path) { $MSBuild = $path; break }
    }
}

if (!$MSBuild -or !(Test-Path $MSBuild)) {
    Write-Error "[build-driver] MSBuild not found. Please install Visual Studio 2022 (Community, Professional, Enterprise, or BuildTools) with 'Desktop development with C++' workload and Windows SDK."
    exit 1
}

Write-AnniLog -Level INFO -Message "Found MSBuild: $MSBuild"

# ---- Build ----------------------------------------------------------------
Write-AnniLog -Level INFO -Message "Building $Config|x64 ..."
& $MSBuild $ProjFile /p:Configuration=$Config /p:Platform=x64 /m /nologo `
           /p:SolutionDir="$RepoRoot\\"

$msbExit = $LASTEXITCODE
if ($msbExit -ne 0) {
    Write-AnniLog -Level ERROR -Message "MSBuild failed (exit $msbExit)."
    Close-AnniLog
    exit $msbExit
}

# ---- Sign driver ----------------------------------------------------------
# Sign the .sys *before* inf2cat so the catalog contains the signed file hash.
$SysFile = "$OutDir\AnniAudioCable.sys"
Write-AnniLog -Level INFO -Message "Signing driver: $SysFile"
& $SignTool sign /sha1 $Thumbprint /fd sha256 `
                 /tr http://timestamp.digicert.com /td sha256 `
                 $SysFile
$sysSignExit = $LASTEXITCODE
if ($sysSignExit -ne 0) {
    Write-AnniLog -Level ERROR -Message "signtool failed on $SysFile (exit $sysSignExit)."
    Close-AnniLog
    exit $sysSignExit
}

# ---- Catalog -------------------------------------------------------------
# inf2cat generates the .cat that PnP requires; output name is lower-case.
Write-AnniLog -Level INFO -Message "Generating catalog (inf2cat) ..."
& $Inf2Cat /driver:$OutDir /os:10_X64
$catExit = $LASTEXITCODE
if ($catExit -ne 0) {
    Write-AnniLog -Level ERROR -Message "inf2cat failed (exit $catExit)."
    Close-AnniLog
    exit $catExit
}

# ---- Sign catalog --------------------------------------------------------
$CatFile = Get-ChildItem $OutDir -Filter "*.cat" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if ($CatFile) {
    Write-AnniLog -Level INFO -Message "Signing catalog: $CatFile"
    & $SignTool sign /sha1 $Thumbprint /fd sha256 `
                     /tr http://timestamp.digicert.com /td sha256 `
                     $CatFile
    $catSignExit = $LASTEXITCODE
    if ($catSignExit -ne 0) {
        Write-AnniLog -Level ERROR -Message "signtool failed on $CatFile (exit $catSignExit)."
        Close-AnniLog
        exit $catSignExit
    }
} else {
    Write-Warning "[build-driver] Expected .cat file not found in $OutDir"
}

Write-AnniLog -Level SUCCESS -Message "Done. Artifacts in: $OutDir"
Close-AnniLog
exit 0
