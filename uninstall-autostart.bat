@echo off
setlocal
rem Removes the hidden-at-logon launcher installed by install-autostart.bat.
rem Does not stop a mixer that is currently running (use stop-mixer.bat).

set APP_DIR=%APPDATA%\AnniAudio
set VBS=%APP_DIR%\AnniAudioMixer.vbs
set LEGACY_VBS=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\AnniAudioMixer.vbs

if exist "%LEGACY_VBS%" (
    del "%LEGACY_VBS%"
    echo Removed legacy startup .vbs: %LEGACY_VBS%
)

if exist "%VBS%" (
    del "%VBS%"
    echo Removed: %VBS%
)

reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v AnniAudioMixer /f >nul 2>&1
if %errorlevel% equ 0 (
    echo Removed Run key: HKCU\...\Run\AnniAudioMixer
) else (
    echo Run key was not installed or could not be removed.
)
