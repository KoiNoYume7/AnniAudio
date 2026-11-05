@echo off
setlocal

:: Anni dev environment setup launcher
:: Double-click this to bootstrap PowerShell 7+ and hand off to dev_setup.ps1.

cd /d "%~dp0"

echo [dev_setup.bat] Working directory: %~dp0
echo [dev_setup.bat] Looking for PowerShell 7+...

:check_pwsh
where pwsh >nul 2>&1
if %errorlevel% == 0 (
    echo [dev_setup.bat] Found pwsh in PATH.
    goto :launch
)

echo.
echo [dev_setup.bat] PowerShell 7+ was not found in PATH.

:: Try winget
echo [dev_setup.bat] Looking for winget...
where winget >nul 2>&1
if %errorlevel% == 0 (
    echo [dev_setup.bat] Found winget in PATH.
    goto :ask_install_pwsh
)

:: No winget either
:ask_winget
echo.
echo [dev_setup.bat] winget is also not installed.
choice /C YN /M "Open the Microsoft Store to install winget (App Installer)"
if %errorlevel% == 2 goto :give_up

start "" "ms-windows-store://pdp/?ProductId=9NBLGGH4NNS1"
echo.
echo [dev_setup.bat] Please install App Installer, then re-run this script.
pause
exit /b 1

:ask_install_pwsh
choice /C YN /M "Install PowerShell 7 with winget now"
if %errorlevel% == 2 goto :give_up

echo [dev_setup.bat] Installing PowerShell 7 via winget...
echo [dev_setup.bat] Running: winget install Microsoft.PowerShell --source winget --accept-package-agreements --accept-source-agreements
winget install Microsoft.PowerShell --source winget --accept-package-agreements --accept-source-agreements
if %errorlevel% neq 0 (
    echo.
    echo [dev_setup.bat] winget install failed with code %errorlevel%. Please install PowerShell 7 manually.
    pause
    exit /b 1
)

:: winget may have installed pwsh into a location not yet in this shell's PATH.
echo [dev_setup.bat] winget finished. Checking common PowerShell install paths...
:: Check the common install paths before giving up.
if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" (
    echo [dev_setup.bat] Found pwsh at: %ProgramFiles%\PowerShell\7\pwsh.exe
    set "PWSH_PATH=%ProgramFiles%\PowerShell\7\pwsh.exe"
    goto :launch_with_path
)
if exist "%ProgramFiles(x86)%\PowerShell\7\pwsh.exe" (
    echo [dev_setup.bat] Found pwsh at: %ProgramFiles(x86)%\PowerShell\7\pwsh.exe
    set "PWSH_PATH=%ProgramFiles(x86)%\PowerShell\7\pwsh.exe"
    goto :launch_with_path
)
if exist "%LOCALAPPDATA%\Microsoft\WindowsApps\pwsh.exe" (
    echo [dev_setup.bat] Found pwsh at: %LOCALAPPDATA%\Microsoft\WindowsApps\pwsh.exe
    set "PWSH_PATH=%LOCALAPPDATA%\Microsoft\WindowsApps\pwsh.exe"
    goto :launch_with_path
)

:: PATH may need a fresh shell; ask the user to re-run.
echo.
echo [dev_setup.bat] PowerShell 7 was installed, but this shell needs to be restarted to see it.
echo Please close this window and re-run dev_setup.bat.
pause
exit /b 1

:launch
echo [dev_setup.bat] Launching dev_setup.ps1 with pwsh...
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev_setup.ps1" %*
exit /b %errorlevel%

:launch_with_path
echo [dev_setup.bat] Launching dev_setup.ps1 with: %PWSH_PATH%
"%PWSH_PATH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev_setup.ps1" %*
exit /b %errorlevel%

:give_up
echo.
echo [dev_setup.bat] Dev setup cancelled.
echo [dev_setup.bat] Please install PowerShell 7 manually from: https://github.com/PowerShell/PowerShell/releases
pause
exit /b 1
