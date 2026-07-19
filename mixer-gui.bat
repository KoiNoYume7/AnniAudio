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
echo  AnniAudio Mixer GUI
echo ===========================================================
echo.
echo  1) Make sure the mixer is running. If not, run start-mixer.bat
echo  2) Opening http://127.0.0.1:8000/scripts/mixer.html?port=8850
echo.

:: Start a tiny local static server so the browser can load the GUI
echo Starting local web server on 127.0.0.1:8000...
start /min "AnniAudio Mixer GUI Server" python -m http.server 8000 --bind 127.0.0.1

:: Give the server a moment to start
timeout /t 2 /nobreak >nul

:: Open the mixer GUI in the default browser
start "" "http://127.0.0.1:8000/scripts/mixer.html?port=8850"

echo GUI opened. Close this window when done.
