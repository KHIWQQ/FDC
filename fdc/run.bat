@echo off
setlocal
cd /d "%~dp0"

set "VENV=.venv"

REM --- locate a Python launcher (py first, then python) ---
set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
  where python >nul 2>&1 && set "PY=python"
)
if not defined PY (
  echo.
  echo [run.bat] ERROR: Python not found on this PC.
  echo   Install Python 3.10+ from https://www.python.org/downloads/
  echo   During setup, TICK the box "Add python.exe to PATH".
  echo.
  pause
  exit /b 1
)
echo [run.bat] Using Python: %PY%

if not exist "%VENV%" (
  echo [run.bat] Creating virtualenv, first run...
  %PY% -m venv "%VENV%"
  if errorlevel 1 goto fail
)

call "%VENV%\Scripts\activate.bat"
if errorlevel 1 goto fail

echo [run.bat] Installing dependencies...
python -m pip install --upgrade pip
pip install -r requirements.txt
if errorlevel 1 goto fail

echo.
echo [run.bat] Starting FDC server at http://localhost:5000  (close window to stop)
echo.
python main.py

echo.
echo [run.bat] Server stopped.
pause
exit /b 0

:fail
echo.
echo [run.bat] *** ERROR - read the message above ***
pause
exit /b 1
