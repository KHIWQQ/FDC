#!/usr/bin/env bash
# build.sh — สร้างไฟล์รันไฟล์เดียว (one-file binary) ด้วย PyInstaller
# ใช้สำหรับแจกจ่ายให้เครื่องที่ไม่มี Python — ดับเบิลคลิกแล้วเปิดเบราว์เซอร์เอง
#   Linux/Mac:  ./build.sh    -> dist/fdc-server
#   Windows  :  รัน build.sh ผ่าน Git Bash หรือใช้คำสั่ง pyinstaller โดยตรง -> dist\fdc-server.exe
set -e
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
VENV=".venv-build"

if [ ! -d "$VENV" ]; then
  "$PY" -m venv "$VENV"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"
pip install --upgrade pip >/dev/null
pip install -r requirements.txt pyinstaller >/dev/null

echo "[build.sh] กำลัง build…"
pyinstaller --clean --noconfirm fdc-server.spec

echo
echo "[build.sh] เสร็จแล้ว -> dist/fdc-server"
echo "          คัดลอกไฟล์นี้ไปเครื่องเป้าหมายแล้วดับเบิลคลิกได้เลย"
