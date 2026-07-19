@echo off
setlocal

cd /d "%~dp0"

if not exist "build\bin\Release\route_cli.exe" (
    if not exist "build\bin\Debug\route_cli.exe" (
        echo WARNING: route_cli.exe not found. Build it first with:
        echo   cmake --build build --target route_cli --config Release
        echo.
    )
)

echo.
echo ===========================================================
echo  AnniAudio Mixer TUI
echo ===========================================================
echo.
echo  Usage: mixer-tui.bat [PORT]
echo  Default port is 8850.
echo.
echo  Make sure the mixer is running. If not, run start-mixer.bat first.
echo.

set PORT=%~1
if "%PORT%"=="" set PORT=8850

chcp 65001 >nul

python -c "import curses" 2>nul
if errorlevel 1 (
    echo Installing windows-curses package...
    python -m pip install windows-curses
    if errorlevel 1 (
        echo Failed to install windows-curses. Please run:
        echo   python -m pip install windows-curses
        pause
        exit /b 1
    )
)

python scripts\mixer_tui.py --port %PORT%
