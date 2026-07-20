@echo off
setlocal
rem Removes the hidden-at-logon launcher installed by install-autostart.bat.
rem Does not stop a mixer that is currently running (use stop-mixer.bat).

set VBS=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\AnniAudioMixer.vbs
if exist "%VBS%" (
    del "%VBS%"
    echo Removed: %VBS%
) else (
    echo Autostart was not installed.
)
