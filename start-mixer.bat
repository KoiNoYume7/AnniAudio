@echo off
setlocal

cd /d "%~dp0"

if not exist "build\bin\Release\route_cli.exe" (
    echo ERROR: route_cli.exe not found. Build it first with:
    echo   cmake --build build --target route_cli --config Release
    pause
    exit /b 1
)

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=config\mixers\default.json

start "AnniAudio Mixer" "build\bin\Release\route_cli.exe" mixer "%CONFIG%" --port 8850
