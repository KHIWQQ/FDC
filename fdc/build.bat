@echo off
REM build.bat - สร้าง fdc-server.exe ไฟล์เดียวสำหรับ Windows (ดับเบิลคลิกใช้ได้เลย)
REM ต้องมี Python 3.10+ บนเครื่องที่ build (เครื่องเป้าหมายไม่ต้องมี)
REM ผลลัพธ์: dist\fdc-server.exe
setlocal
cd /d "%~dp0"

set "PY=python"
set "VENV=.venv-build"

if not exist "%VENV%" (
  echo [build] creating build venv...
  %PY% -m venv "%VENV%"
)
call "%VENV%\Scripts\activate.bat"
python -m pip install --upgrade pip >nul
pip install -r requirements.txt pyinstaller

echo [build] building one-file exe...
pyinstaller --clean --noconfirm fdc-server.spec

echo.
echo [build] done -^> dist\fdc-server.exe
echo         copy that single file to any Windows PC and double-click it.
echo         (it auto-detects the dongle and opens the browser by itself)
pause
