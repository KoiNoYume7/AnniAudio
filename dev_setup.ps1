#Requires -Version 7.0

# Anni Dev Environment Setup
# --------------------------
# Template for bootstrapping a fresh Windows dev machine.
# Copy `dev_setup.bat`, `dev_setup.ps1` and `.assets/` to a project root and edit
# the `$script:Components` and `$script:PythonPackages` lists below.
#
# Usage:
#   .\dev_setup.bat                (interactive, prompts before each install)
#   .\dev_setup.ps1 -Unattended    (install without asking, no pause at end)
#   .\dev_setup.ps1 -CheckOnly     (report what is missing, do not install)
#   .\dev_setup.ps1 -Quiet         (INFO-level log only, hides DEBUG detail)

[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$Unattended,
    [switch]$Quiet
)

# ------- Section: Self-elevation ------- #

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin) -and -not $CheckOnly) {
    Write-Host 'This setup needs administrator privileges. Requesting elevation...' -ForegroundColor Yellow
    $argList = $MyInvocation.BoundParameters.GetEnumerator() | ForEach-Object {
        $k = $_.Key
        $v = $_.Value
        if ($v -is [switch]) { "-$k" } else { "-$k `"$v`"" }
    }
    Start-Process -FilePath 'pwsh' -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" $argList" -Verb runAs
    exit
}

$ErrorActionPreference = 'Stop'

# ------- Section: AnniLog bootstrap ------- #

$anniLogPath = Join-Path $PSScriptRoot '.assets' 'AnniLog.psm1'
if (Test-Path $anniLogPath) {
    Import-Module $anniLogPath
} else {
    # Console-only fallback so the script still runs if the module is missing.
    function Write-AnniLog { param([string]$Level = 'INFO', [string]$Message) Write-Host "[$Level] $Message" }
    function Close-AnniLog {}
    Write-Warning "AnniLog module not found at $anniLogPath; logging to console only."
}

$script:ProjectName = 'AnniAudio'     # Change to your project name
$logDir = Join-Path $PSScriptRoot 'logs'
$script:LogFile = Join-Path $logDir "dev-setup-$($script:ProjectName.ToLower())-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
$logLevel = if ($Quiet) { 'INFO' } else { 'DEBUG' }
Initialize-AnniLog -LogFilePath $script:LogFile -LogLevel $logLevel -EnableStopwatch

Write-AnniLog -Level DEBUG -Message "PSScriptRoot: $PSScriptRoot"
Write-AnniLog -Level DEBUG -Message "CheckOnly: $CheckOnly"
Write-AnniLog -Level DEBUG -Message "Unattended: $Unattended"
Write-AnniLog -Level DEBUG -Message "Quiet: $Quiet"

# ------- Section: Helpers ------- #

function Find-DevExe {
    <#
    .SYNOPSIS
        Finds an executable on PATH or in standard install locations.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [string[]]$Candidates
    )

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-AnniLog -Level DEBUG -Message "$Name found on PATH: $($cmd.Source)"
        return $cmd.Source
    }

    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            Write-AnniLog -Level DEBUG -Message "$Name not on PATH, found at: $candidate"
            return $candidate
        }
    }

    Write-AnniLog -Level DEBUG -Message "$Name not found on PATH or in: $($Candidates -join ', ')"
    return $null
}

# ------- Section: Project configuration ------- #

# Edit this list to match the stack your project needs.
# Each component needs:
#   Name        - Human-readable name
#   WingetId    - winget package ID (used for install)
#   Test        - ScriptBlock that returns $true if the component is already present
$script:Components = @(
    @{
        Name     = 'Git'
        WingetId = 'Git.Git'
        Test     = {
            $git = Find-DevExe 'git' @(
                "$env:ProgramFiles\Git\cmd\git.exe",
                "${env:ProgramFiles(x86)}\Git\cmd\git.exe"
            )
            if (-not $git) { return $false }
            $ver = & $git --version 2>&1
            Write-AnniLog -Level DEBUG -Message "git --version returned: $ver"
            return $true
        }
    }
    @{
        Name     = 'Python 3'
        WingetId = 'Python.Python.3.13'   # or 'Microsoft.Python.3.12' if you prefer
        Test     = {
            $py = Get-Command python -ErrorAction SilentlyContinue
            if ($py) {
                $pyExe = $py.Source
            } else {
                $pyExe = Get-ChildItem "$env:LocalAppData\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue |
                    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
                if (-not $pyExe) {
                    $pyExe = Get-ChildItem "${env:ProgramFiles}\Python3*\python.exe" -ErrorAction SilentlyContinue |
                        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
                }
            }
            if (-not $pyExe) { Write-AnniLog -Level DEBUG -Message 'python not found on PATH or in standard install dirs'; return $false }
            Write-AnniLog -Level DEBUG -Message "python found at: $pyExe"
            $ver = & $pyExe --version 2>&1 | Select-String -Pattern '(\d+)\.(\d+)'
            Write-AnniLog -Level DEBUG -Message "python --version returned: $ver"
            return [int]$ver.Matches.Groups[1].Value -ge 3
        }
    }
    @{
        Name     = 'Node.js'
        WingetId = 'OpenJS.NodeJS.LTS'
        Test     = {
            $node = Find-DevExe 'node' @(
                "$env:ProgramFiles\nodejs\node.exe",
                "${env:ProgramFiles(x86)}\nodejs\node.exe"
            )
            if (-not $node) { return $false }
            $ver = & $node --version 2>&1
            Write-AnniLog -Level DEBUG -Message "node --version returned: $ver"
            return $true
        }
    }
    @{
        Name     = '.NET 8 SDK'
        WingetId = 'Microsoft.DotNet.SDK.8'
        Test     = {
            $dotnet = Find-DevExe 'dotnet' @(
                "$env:ProgramFiles\dotnet\dotnet.exe",
                "${env:ProgramFiles(x86)}\dotnet\dotnet.exe"
            )
            if (-not $dotnet) { return $false }
            $sdks = & $dotnet --list-sdks 2>&1
            Write-AnniLog -Level DEBUG -Message "dotnet --list-sdks returned:`n$($sdks -join "`n")"
            return [bool]($sdks | Select-String -Pattern '^8\.')
        }
    }
    @{
        Name     = 'Visual Studio Build Tools 2022 (C++)'
        WingetId = 'Microsoft.VisualStudio.2022.BuildTools'
        Test     = {
            # The bare BuildTools package installs no workloads; C++ builds need the
            # VC toolset, so check for the component, not just any VS instance.
            $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
            if (-not (Test-Path $vsWhere)) { Write-AnniLog -Level DEBUG -Message "vswhere.exe not found at $vsWhere"; return $false }
            $installs = & $vsWhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
            Write-AnniLog -Level DEBUG -Message "vswhere (with VC.Tools) found $($installs.Count) installation(s): $(($installs | ForEach-Object { $_.displayName }) -join ', ')"
            return [bool]($installs -and $installs.Count -gt 0)
        }
        Install  = {
            # If a VS instance exists but lacks the C++ workload, modify it in place;
            # otherwise install BuildTools with the workload via winget --override.
            $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
            $setup   = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
            $workload = 'Microsoft.VisualStudio.Workload.VCTools'

            $installs = @(& $vsWhere -products * -format json | ConvertFrom-Json)
            if ($installs.Count -gt 0 -and (Test-Path $setup)) {
                $path = $installs[0].installationPath
                Write-AnniLog -Level INFO -Message "VS instance found at $path - adding VCTools workload via setup.exe modify"
                # NB: setup.exe modify does NOT accept --wait (bootstrapper-only arg).
                $setupArgs = @('modify', '--installPath', $path, '--add', $workload, '--includeRecommended', '--norestart')
                $setupArgs += if ($Unattended) { '--quiet' } else { '--passive' }
                $code = Invoke-DevCommand -FilePath $setup -ArgumentList $setupArgs
                return ($code -eq 0)
            }

            $code = Invoke-DevCommand -FilePath 'winget' -ArgumentList @(
                'install', '--id', 'Microsoft.VisualStudio.2022.BuildTools',
                '--source', 'winget',
                '--accept-package-agreements', '--accept-source-agreements',
                '--override', "--wait --quiet --add $workload --includeRecommended --norestart",
                '--verbose-logs'
            )
            return ($code -eq 0)
        }
    }
    @{
        Name     = 'Windows SDK'
        WingetId = 'Microsoft.WindowsSDK.10.0.26100'  # Adjust to latest stable ID as needed
        Test     = {
            $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
            $found = Test-Path $kitRoot
            if ($found) { Write-AnniLog -Level DEBUG -Message "Windows SDK found at $kitRoot" } else { Write-AnniLog -Level DEBUG -Message "Windows SDK not found at $kitRoot" }
            return $found
        }
    }
    @{
        Name     = 'CMake'
        WingetId = 'Kitware.CMake'
        Test     = {
            $cmake = Find-DevExe 'cmake' @(
                "$env:ProgramFiles\CMake\bin\cmake.exe",
                "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
            )
            if (-not $cmake) { return $false }
            $ver = & $cmake --version 2>&1 | Select-Object -First 1
            Write-AnniLog -Level DEBUG -Message "cmake --version returned: $ver"
            return $true
        }
    }
)

# Optional: Python packages installed into the active Python environment after Python is present.
# Example: @('windows-curses', 'requests')
# NOTE: winappaudiorouter is intentionally absent — it is not published on PyPI.
$script:PythonPackages = @('windows-curses', 'pyflakes')

# ------- Section: External command runner ------- #

function Invoke-DevCommand {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter(Mandatory)]
        [string[]]$ArgumentList
    )

    $cmdLine = "$FilePath $($ArgumentList -join ' ')"
    Write-AnniLog -Level INFO -Message "Running: $cmdLine"

    # Stream the command's output through Write-AnniLog so it lands on the
    # console AND inside the log file.
    & $FilePath @ArgumentList 2>&1 | ForEach-Object {
        $line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { $_ }
        Write-AnniLog -Level INFO -Message "  $line"
    }

    return $LASTEXITCODE
}

# ------- Section: Component checks & install ------- #

function Test-DevComponent {
    param([hashtable]$Component)
    Write-AnniLog -Level INFO -Message "Checking $($Component.Name)..."
    $result = & $Component.Test
    Write-AnniLog -Level DEBUG -Message "Test for $($Component.Name) returned: $result"
    return $result
}

function Install-DevComponent {
    param([hashtable]$Component)

    if (-not $Unattended -and -not $CheckOnly) {
        $choice = Read-Host "Install $($Component.Name) via winget? [Y/n]"
        if ($choice -and $choice.ToLower() -eq 'n') {
            Write-AnniLog -Level WARNING -Message "$($Component.Name) skipped by user."
            return $false
        }
    }

    if ($CheckOnly) {
        Write-AnniLog -Level WARNING -Message "$($Component.Name) is missing. Use -Unattended to install."
        return $false
    }

    Write-AnniLog -Level INFO -Message "Installing $($Component.Name) ($($Component.WingetId))..."
    try {
        if ($Component.Install) {
            $result = & $Component.Install
            Write-AnniLog -Level DEBUG -Message "Install for $($Component.Name) returned: $result"
            return [bool]$result
        }

        $wingetArgs = @(
            'install',
            '--id', $Component.WingetId,
            '--source', 'winget',
            '--accept-package-agreements',
            '--accept-source-agreements',
            '--verbose-logs'
        )

        if ($Unattended) {
            $wingetArgs += '--silent'
        }

        $exitCode = Invoke-DevCommand -FilePath 'winget' -ArgumentList $wingetArgs

        if ($exitCode -ne 0) {
            Write-AnniLog -Level ERROR -Message "winget install for $($Component.Name) returned $exitCode."
            return $false
        }

        Write-AnniLog -Level SUCCESS -Message "$($Component.Name) installed."
        return $true
    }
    catch {
        Write-AnniLog -Level ERROR -Message "Failed to install $($Component.Name): $_"
        return $false
    }
}

function Install-PythonPackages {
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        Write-AnniLog -Level WARNING -Message 'Python not found; skipping Python packages.'
        return
    }

    if ($script:PythonPackages.Count -eq 0) {
        Write-AnniLog -Level INFO -Message 'No Python packages configured in $PythonPackages.'
        return
    }

    if (-not $Unattended -and -not $CheckOnly) {
        $pkgList = $script:PythonPackages -join ', '
        $choice = Read-Host "Install Python packages ($pkgList)? [Y/n]"
        if ($choice -and $choice.ToLower() -eq 'n') {
            Write-AnniLog -Level WARNING -Message 'Python packages skipped by user.'
            return
        }
    }

    if ($CheckOnly) {
        Write-AnniLog -Level INFO -Message "Python packages to install: $($script:PythonPackages -join ', ')"
        return
    }

    foreach ($pkg in $script:PythonPackages) {
        $exitCode = Invoke-DevCommand -FilePath 'python' -ArgumentList @('-m', 'pip', 'install', $pkg)
        if ($exitCode -ne 0) {
            Write-AnniLog -Level ERROR -Message "pip install $pkg failed."
        } else {
            Write-AnniLog -Level SUCCESS -Message "pip install $pkg completed."
        }
    }
}

# ------- Section: Main ------- #

$exitCode = 0

try {
    # Banner
    $artPath = Join-Path $PSScriptRoot '.assets' 'AnniArt.ps1'
    Write-AnniLog -Level DEBUG -Message "Looking for AnniArt at: $artPath"
    if (Test-Path $artPath) {
        . $artPath
        Show-AnniArt
    } else {
        Write-AnniLog -Level WARNING -Message 'AnniArt not found; skipping banner.'
    }

    Write-Host ''
    Write-AnniLog -Level INFO -Message "Setting up dev environment for $ProjectName..."
    Write-AnniLog -Level INFO -Message "Logging to: $LogFile"

    if ($CheckOnly)   { Write-AnniLog -Level INFO -Message 'Running in check-only mode.' }
    if ($Unattended)  { Write-AnniLog -Level INFO -Message 'Running unattended; installs will not prompt.' }
    if ($Quiet)       { Write-AnniLog -Level INFO -Message 'Running quiet; DEBUG detail suppressed.' }

    $present = 0
    $missing = 0
    $installed = 0
    $skipped = 0

    foreach ($component in $script:Components) {
        if (Test-DevComponent -Component $component) {
            Write-AnniLog -Level SUCCESS -Message "$($component.Name) is already present."
            $present++
        } else {
            $missing++
            $result = Install-DevComponent -Component $component
            if ($result) {
                $installed++
            } else {
                $skipped++
            }
        }
    }

    Install-PythonPackages

    # ------- Section: Summary ------- #

    Write-Host ''
    Write-AnniLog -Level INFO -Message '--- Setup summary ---'
    Write-AnniLog -Level INFO -Message "Already present : $present"
    Write-AnniLog -Level INFO -Message "Missing         : $missing"
    Write-AnniLog -Level INFO -Message "Installed       : $installed"
    Write-AnniLog -Level INFO -Message "Skipped/failed  : $skipped"
    Write-AnniLog -Level INFO -Message "Log file        : $LogFile"

    if ($skipped -gt 0) {
        Write-AnniLog -Level WARNING -Message 'Some components were skipped or failed. Check the log above.'
        $exitCode = 1
    } else {
        Write-AnniLog -Level SUCCESS -Message 'Dev environment is ready.'
    }
}
catch {
    Write-AnniLog -Level ERROR -Message "Unhandled error: $_"
    $exitCode = 1
}
finally {
    Close-AnniLog
    if (-not $Unattended -and [Environment]::UserInteractive) {
        try { Read-Host 'Press Enter to close' | Out-Null } catch {}
    }
}

exit $exitCode
