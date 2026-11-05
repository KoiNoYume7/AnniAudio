# AnniLog -- Reusable PowerShell Logging Module
# Provides structured, levelled logging with dual output (console + file).
# No project-specific assumptions -- usable in any PowerShell 7+ project.
#
# Usage:
#   Import-Module "$PSScriptRoot\AnniLog.psm1"
#   Initialize-AnniLog -LogFilePath "C:\logs\myapp.log" -LogLevel "INFO"
#   Write-AnniLog -Level INFO -Message "Hello world"
#   Close-AnniLog

# ------- Module State ------- #

$script:AnniLogState = @{
    LogFilePath  = $null
    LogLevel     = "INFO"
    Stopwatch    = $null
    Initialised  = $false
}

# Numeric priority for each log level (lower = more severe)
$script:LevelPriority = @{
    "ERROR"   = 0
    "WARNING" = 1
    "INFO"    = 2
    "SUCCESS" = 2
    "DEBUG"   = 3
}

# Console colours for each log level
$script:LevelColour = @{
    "ERROR"   = "Red"
    "WARNING" = "Yellow"
    "INFO"    = "White"
    "SUCCESS" = "Green"
    "DEBUG"   = "Cyan"
}

# ------- Public Functions ------- #

function Initialize-AnniLog {
    <#
    .SYNOPSIS
        Initialises the logging session.
    .DESCRIPTION
        Sets the log file path, log level, and starts an optional stopwatch.
        Must be called before Write-AnniLog.
    .PARAMETER LogFilePath
        Full path to the log file. Parent directory is created if missing.
    .PARAMETER LogLevel
        Minimum level to output. Messages below this level are suppressed.
        Valid values: ERROR, WARNING, INFO, SUCCESS, DEBUG.
        Default: INFO.
    .PARAMETER EnableStopwatch
        If set, starts a stopwatch that can be referenced in Close-AnniLog.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$LogFilePath,

        [ValidateSet("ERROR", "WARNING", "INFO", "SUCCESS", "DEBUG")]
        [string]$LogLevel = "INFO",

        [switch]$EnableStopwatch
    )

    $logDir = Split-Path -Parent $LogFilePath
    if ($logDir -and -not (Test-Path $logDir)) {
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    }

    $script:AnniLogState.LogFilePath = $LogFilePath
    $script:AnniLogState.LogLevel    = $LogLevel
    $script:AnniLogState.Initialised = $true

    if ($EnableStopwatch) {
        $script:AnniLogState.Stopwatch = [System.Diagnostics.Stopwatch]::new()
        $script:AnniLogState.Stopwatch.Start()
    }

    # Write session header to log file
    $header = "--- Log session started at $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ---"
    $header | Out-File -FilePath $LogFilePath -Append -Encoding utf8
}

function Write-AnniLog {
    <#
    .SYNOPSIS
        Writes a log message at the specified level.
    .DESCRIPTION
        Outputs to both console (coloured) and log file (timestamped).
        Messages below the configured log level are suppressed.
    .PARAMETER Level
        Log level for this message.
    .PARAMETER Message
        The message text to log.
    #>
    [CmdletBinding()]
    param(
        [ValidateSet("ERROR", "WARNING", "INFO", "SUCCESS", "DEBUG")]
        [string]$Level = "INFO",

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $script:AnniLogState.Initialised) {
        Write-Warning "AnniLog: Write-AnniLog called before Initialize-AnniLog. Message discarded."
        return
    }

    # Check if this message's level is within the configured threshold
    $msgPriority = $script:LevelPriority[$Level]
    $cfgPriority = $script:LevelPriority[$script:AnniLogState.LogLevel]

    if ($msgPriority -gt $cfgPriority) {
        return
    }

    $timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $consoleLine = "[$Level] $Message"
    $fileLine    = "$timestamp [$Level] $Message"

    # Console output with colour
    $colour = $script:LevelColour[$Level]
    if ($colour) {
        Write-Host $consoleLine -ForegroundColor $colour
    } else {
        Write-Host $consoleLine
    }

    # File output with timestamp
    if ($script:AnniLogState.LogFilePath) {
        $fileLine | Out-File -FilePath $script:AnniLogState.LogFilePath -Append -Encoding utf8
    }
}

function Close-AnniLog {
    <#
    .SYNOPSIS
        Closes the logging session.
    .DESCRIPTION
        Stops the stopwatch (if running), writes elapsed time to log, and
        writes a session footer. Call this at the end of your script.
    #>
    [CmdletBinding()]
    param()

    if (-not $script:AnniLogState.Initialised) {
        return
    }

    $elapsed = $null
    if ($script:AnniLogState.Stopwatch) {
        $script:AnniLogState.Stopwatch.Stop()
        $elapsed = $script:AnniLogState.Stopwatch.Elapsed
        Write-AnniLog -Level INFO -Message ("Total elapsed time: {0:hh\:mm\:ss\.fff}" -f $elapsed)
    }

    $footer = "--- Log session ended at $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ---"
    if ($script:AnniLogState.LogFilePath) {
        $footer | Out-File -FilePath $script:AnniLogState.LogFilePath -Append -Encoding utf8
    }

    # Reset state
    $script:AnniLogState.LogFilePath  = $null
    $script:AnniLogState.LogLevel     = "INFO"
    $script:AnniLogState.Stopwatch    = $null
    $script:AnniLogState.Initialised  = $false
}

# ------- Module Exports ------- #

Export-ModuleMember -Function Initialize-AnniLog, Write-AnniLog, Close-AnniLog
