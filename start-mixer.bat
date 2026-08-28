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
if "%CONFIG%"=="" (
    if not exist "config\mixers\main.json" (
        if exist "config\mixers\default.json" (
            copy /Y "config\mixers\default.json" "config\mixers\main.json" >nul
        ) else (
            echo ERROR: No mixer config found. Create config\mixers\main.json or default.json.
            pause
            exit /b 1
        )
    )
    set CONFIG=config\mixers\main.json
)

echo Starting mixer with %CONFIG% on port 8850...
start "AnniAudio Mixer" "build\bin\Release\route_cli.exe" mixer "%CONFIG%" --port 8850
