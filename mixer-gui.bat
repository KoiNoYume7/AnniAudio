@echo off
setlocal

cd /d "%~dp0"

:: Make sure node/npm are available without the user editing PATH.
where node >nul 2>&1
if %errorlevel% neq 0 set "PATH=%PATH%;C:\Program Files\nodejs"

:: Some shells (including Devin) set this and break Electron's main process.
set "ELECTRON_RUN_AS_NODE="

cd gui

if not exist "node_modules" (
  echo Installing GUI dependencies one time...
  call npm install
  if %errorlevel% neq 0 (
    echo GUI dependency install failed.
    pause
    exit /b 1
  )
)

call npm start
