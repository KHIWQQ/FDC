# PyInstaller spec — สร้างไฟล์เดียว (one-file) สำหรับ FDC Web App
# build:  pyinstaller fdc-server.spec
# ได้ผลลัพธ์:  dist/fdc-server      (Linux/Mac)
#             dist/fdc-server.exe  (Windows)
#
# templates/ ถูกฝังเข้าไปในไฟล์เดียว แล้วแตกออกที่ sys._MEIPASS ตอนรัน
# (main.py รองรับแล้วผ่าน getattr(sys, "frozen", False))

# -*- mode: python ; coding: utf-8 -*-
import os

block_cipher = None

# datas: (ที่มา, ปลายทางภายใน bundle)
datas = [
    ("templates", "templates"),
]
# config.yaml — ฝังไปด้วยถ้ามี (main.py อ่านจาก sys._MEIPASS ตอน frozen)
if os.path.isfile("config.yaml"):
    datas.append(("config.yaml", "."))
# ถ้ามีโฟลเดอร์ static ในอนาคต ให้ฝังไปด้วยอัตโนมัติ
if os.path.isdir("static"):
    datas.append(("static", "static"))

# hidden imports: pyserial backends ทุก OS + optional libs (ใส่เฉพาะตัวที่ลงไว้)
import importlib.util
_hidden = [
    "serial",
    "serial.tools.list_ports",
    "serial.tools.list_ports_linux",
    "serial.tools.list_ports_windows",
    "serial.tools.list_ports_osx",
]
for _opt in ("pyudev", "yaml", "paho.mqtt.client"):
    if importlib.util.find_spec(_opt) is not None:
        _hidden.append(_opt)

a = Analysis(
    ["main.py"],
    pathex=[],
    binaries=[],
    datas=datas,
    # ไลบรารีที่โหลดแบบ dynamic — บอก PyInstaller ให้เก็บมาให้ครบ
    hiddenimports=_hidden,
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="fdc-server",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,        # เปิด console ไว้เพื่อดู log/พิมพ์ URL
    disable_windowed_traceback=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
