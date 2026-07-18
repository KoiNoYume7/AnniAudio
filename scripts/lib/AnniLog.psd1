@{
    RootModule        = 'AnniLog.psm1'
    ModuleVersion     = '1.0.0'
    GUID              = '51043702-67e4-4abe-8567-49efe5f52f51'
    Author            = 'KoiNoYume7'
    Description       = 'Reusable structured logging module with levelled console and file output.'
    PowerShellVersion = '7.0'
    FunctionsToExport = @(
        'Initialize-AnniLog',
        'Write-AnniLog',
        'Close-AnniLog'
    )
    CmdletsToExport   = @()
    VariablesToExport  = @()
    AliasesToExport    = @()
}
