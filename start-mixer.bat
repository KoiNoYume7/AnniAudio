@echo off
setlocal

cd /d "%~dp0"

if exist "build\bin\Release\route_cli.exe" (
    set "MIXER_EXE=build\bin\Release\route_cli.exe"
) else if exist "build\bin\Debug\route_cli.exe" (
    set "MIXER_EXE=build\bin\Debug\route_cli.exe"
) else (
    echo route_cli.exe not found. Build it first with:
    echo   cmake --build build --target route_cli --config Release
    pause
    exit /b 1
)

echo.
echo Starting mixer + control API using config/mixers/default.json
echo.

start "AnniAudio Mixer" "%MIXER_EXE%" mixer config/mixers/default.json --port 8850

echo Mixer started in a new window.
echo.
echo Now run mixer-gui.bat to open the web interface.
