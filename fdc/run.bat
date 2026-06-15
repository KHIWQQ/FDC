@echo off
REM run.bat - run FDC Web App on Windows (double-click). First run sets up venv + deps.
setlocal
cd /d "%~dp0"

set "PY=python"
set "VENV=.venv"

if not exist "%VENV%" (
  echo [run.bat] Creating virtualenv (first run)...
  %PY% -m venv "%VENV%"
  call "%VENV%\Scripts\activate.bat"
  python -m pip install --upgrade pip >nul
  pip install -r requirements.txt
) else (
  call "%VENV%\Scripts\activate.bat"
)

echo [run.bat] Starting FDC server at http://localhost:5000  (close window to stop)
python main.py

pause
