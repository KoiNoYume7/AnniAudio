@echo off
setlocal
rem Stops the running mixer. Safe to force-kill: the mixer autosaves its
rem config on every change, so no state is lost. Handy before rebuilding
rem (a running route_cli.exe locks the file the linker wants to write).

taskkill /IM route_cli.exe /F >nul 2>&1
if errorlevel 1 (
    echo Mixer was not running.
) else (
    echo Mixer stopped.
)
