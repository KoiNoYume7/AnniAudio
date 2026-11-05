# Anni Dev Environment Setup Template

A reusable, self-contained Windows dev-environment bootstrap for Anni projects.

## What it does

1. `dev_setup.bat` (double-click friendly) checks for **PowerShell 7+**.
2. If PowerShell 7 is missing, it tries to install it via **winget**.
3. If winget is missing, it offers to open the Microsoft Store to the **App Installer** page and exits so you can install winget and re-run.
4. Once PowerShell 7 is available, it launches `dev_setup.ps1`.
5. `dev_setup.ps1` displays the **AnniArt** banner and then checks/installs the configured toolchain.

## Files

| File | Purpose |
|---|---|
| `dev_setup.bat` | Entry point. Handles PowerShell 7 detection/installation, then launches the `.ps1` and exits. The `.ps1` holds the window open. |
| `dev_setup.ps1` | Main setup logic. Checks configured tools and installs missing ones via winget. Uses `AnniLog` for console/file logging. |
| `.assets/AnniArt.ps1` | ASCII banner (`Show-AnniArt`) shown at the start of `dev_setup.ps1`. |
| `.assets/AnniLog.psm1` | Shared logging module (`Initialize-AnniLog`, `Write-AnniLog`, `Close-AnniLog`). Logs are written to `logs/` next to `dev_setup.ps1`. |
| `README.md` | This file. |

## How to use in a new project

1. Copy these files to the project root:

   ```text
   project-root/
   ├── dev_setup.bat
   ├── dev_setup.ps1
   ├── .assets/
   │   ├── AnniArt.ps1
   │   └── AnniLog.psm1
   └── ...
   ```

2. Edit `dev_setup.ps1`:
   - Set `$script:ProjectName` to the project name.
   - Adjust `$script:Components` to match the project stack.
   - Add any required `pip` packages to `$script:PythonPackages`.

3. Double-click `dev_setup.bat`.

## Running modes

```powershell
# Interactive (default): prompts before each install, verbose output
.\dev_setup.ps1

# Unattended: installs without asking
.\dev_setup.ps1 -Unattended

# Check-only: reports what is missing, no installs, no elevation required
.\dev_setup.ps1 -CheckOnly

# Suppress verbose output
.\dev_setup.ps1 -Quiet
```

The batch file always passes its arguments through, so you can also run:

```text
dev_setup.bat -CheckOnly
dev_setup.bat -Unattended
dev_setup.bat -Quiet
```

## Default components

The template ships with these checks (edit as needed):

- Git
- Python 3
- Node.js LTS
- .NET 8 SDK
- Visual Studio Build Tools 2022 (with the C++/VCTools workload — a bare BuildTools install does not count; the test requires `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`)
- Windows SDK
- CMake

## Notes

- `dev_setup.ps1` will self-elevate via UAC if it is not already running as Administrator, because some installs (Windows SDK, Build Tools) require elevation. If you prefer a non-elevated setup, remove the `Test-IsAdmin` block from the top of `dev_setup.ps1`.
- `winget` package IDs can drift; verify them with `winget search <name>` before using the template on a fresh machine.
- The log is written to `logs\dev-setup-<projectname>-<timestamp>.log` inside the project root.
- `dev_setup.ps1` pauses at the end (`Press Enter to close`) so the console window stays open on success or error; `-Unattended` skips the pause for automation. `dev_setup.bat` exits right after handing off.
- `dev_setup.bat` and `dev_setup.ps1` stream `winget`/`pip` output directly to the console and into the log via `Write-AnniLog`.
- The AnniArt banner uses 24-bit ANSI colour and requires a modern terminal (Windows Terminal, VS Code terminal, or PowerShell 7 conhost).
