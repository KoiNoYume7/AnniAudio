<#
.SYNOPSIS
    Restore the default Windows playback (render) endpoint.
.DESCRIPTION
    Uses route_cli default to switch the default render endpoint back to a
    physical device such as headphones or speakers. This is the safe fallback
    command after routing an app through the AnniAudio virtual cable.

    If the hint matches more than one endpoint, the first match is used.

    Usage:
      .\scripts\restore-default.ps1 [DeviceHint]
      .\cli\anniaudio.ps1 restore-default [DeviceHint]

    Examples:
      .\scripts\restore-default.ps1
      .\scripts\restore-default.ps1 "Headphones (Crusher ANC 2)"
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Hint = "Headphones"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path "$PSScriptRoot\.."
$RouteCli = "$RepoRoot\build\bin\Release\route_cli.exe"

if (!(Test-Path $RouteCli)) {
    Write-Error "route_cli.exe not found at $RouteCli. Build the project in Release first."
    exit 1
}

Write-Host "[restore-default] Restoring default playback to endpoint matching: $Hint" -ForegroundColor Cyan
& $RouteCli default $Hint
$exit = $LASTEXITCODE
if ($exit -ne 0) {
    Write-Host "[restore-default] No endpoint matched '$Hint'. Run one of the following:" -ForegroundColor Yellow
    Write-Host "  route_cli list" -ForegroundColor Yellow
    Write-Host "  .\cli\anniaudio.ps1 restore-default '<exact name>'" -ForegroundColor Yellow
    exit $exit
}
Write-Host "[restore-default] Done." -ForegroundColor Green
