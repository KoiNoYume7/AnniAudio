@{
    RootModule             = "AudioConfig.dll"
    ModuleVersion          = '1.0.0'
    CompatiblePSEditions   = @("Core", "Desktop")
    GUID                   = '9943c92e-c7e2-487e-87f3-8604a811de00'
    Author                 = 'MartinGC94'
    CompanyName            = 'Unknown'
    Copyright              = '(c) 2025 MartinGC94. All rights reserved.'
    Description            = 'Manage Windows audio devices and sessions.'
    PowerShellVersion      = '5.1'
    TypesToProcess         = @()
    FormatsToProcess       = @('AudioConfigFormat.ps1xml')
    FunctionsToExport      = @()
    CmdletsToExport        = @('Get-AudioDevice','Get-AudioSession','Set-AudioDevice','Set-AudioSession')
    VariablesToExport      = @()
    AliasesToExport        = @()
    DscResourcesToExport   = @()
    FileList               = @('AudioConfig.deps.json','AudioConfig.dll','AudioConfig.psd1','AudioConfigFormat.ps1xml','en-US\AudioConfig.dll-Help.xml')
    PrivateData            = @{
        PSData = @{
             Tags         = @("Audio", "Sound", "Volume", "Config", "Settings")
             ProjectUri   = 'https://github.com/MartinGC94/AudioConfig'
             ReleaseNotes = @'
1.0.0:
    Initial release.
'@
        }
    }
}
