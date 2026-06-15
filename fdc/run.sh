#!/usr/bin/env bash
# run.sh — รัน FDC Web App บน Linux/Mac แบบคลิกเดียว (ไม่ต้องแพ็กเกจ)
# ครั้งแรกจะสร้าง virtualenv + ติดตั้ง dependency ให้อัตโนมัติ แล้วเปิดเบราว์เซอร์เอง
set -e
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
VENV=".venv"

if [ ! -d "$VENV" ]; then
  echo "[run.sh] สร้าง virtualenv ครั้งแรก…"
  "$PY" -m venv "$VENV"
  # shellcheck disable=SC1091
  source "$VENV/bin/activate"
  pip install --upgrade pip >/dev/null
  pip install -r requirements.txt
else
  # shellcheck disable=SC1091
  source "$VENV/bin/activate"
fi

echo "[run.sh] เริ่ม FDC server ที่ http://localhost:5000 (Ctrl+C เพื่อหยุด)"
exec python main.py
