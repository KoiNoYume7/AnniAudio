@echo off
rem Removes the AnniAudio autostart (scheduled task + Task Manager entry +
rem launcher). Thin wrapper around: install-autostart.bat uninstall
rem Does not stop a mixer that is currently running (use stop-mixer.bat).
cd /d "%~dp0"
call "%~dp0install-autostart.bat" uninstall
