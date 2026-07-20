"""
demo_run.py — เปิด FDC Web UI แบบสาธิต (ไม่ต้องมี LoRa dongle / GPS / MQTT)

ใส่ข้อมูลตัวอย่างลง state โดยตรง: FDC มี fix + ปืน 4 กระบอกรอบ ศป.(ลพบุรี)
เพื่อให้ลองฟีเจอร์ได้ครบ — MOVE GUN, CEL/FIRE, หลัก/สำรอง, ที่ตั้งยิง, สลับชั้นแผนที่
*ไม่แก้ main.py* — เป็นแค่ตัวรันสาธิต

รัน:
  cd /home/ubuntu/FDC
  PYTHONPATH=venv/lib/python3.12/site-packages FDC_NO_BROWSER=1 python3 fdc/demo_run.py
แล้วเปิด http://localhost:5000
"""
import os, time, threading

os.environ.setdefault("FDC_NO_BROWSER", "1")   # headless — อย่าเปิดเบราว์เซอร์เอง
import main

# ── ข้อมูลตัวอย่าง: FDC + ปืน 4 กระบอก (ใกล้ ศป. ลพบุรี ~14.795N,100.652E) ──
BASE_LAT, BASE_LON, BASE_ALT = 14.7950, 100.6520, 12.0
GUN_OFFSETS = [   # (dlat, dlon, heading°)  กระจายรอบ FDC
    (+0.0040, +0.0030, 18),
    (+0.0062, -0.0021, 28),
    (-0.0031, +0.0049, 33),
    (-0.0052, -0.0040, 52),
]

with main.state_lock:
    main.state["fdc"].update(lat=BASE_LAT, lon=BASE_LON, alt=BASE_ALT,
                             fix=3, sats=11, valid=True)
    for i, (dla, dlo, hdg_deg) in enumerate(GUN_OFFSETS, start=1):
        main.state["guns"][str(i)].update(
            alive=True,
            lat=BASE_LAT + dla, lon=BASE_LON + dlo,
            az=0, el=0,
            hdg=int(hdg_deg * 6400 / 360),
            bat=92 - i * 7, rssi=-58 - i * 4, snr=9.0,
            last_seen=time.time(),
        )
    # gun #1 = ที่ตั้งหลัก อยู่แล้วโดย default (role=primary)

# ไม่มี broker จริง — กัน uplink.publish ไม่ให้พยายามส่ง
main.uplink.publish = lambda *a, **k: None

# refresh last_seen + push SSE เป็นระยะ ไม่งั้น UI จะ mark ปืน OFFLINE หลัง 30s
def _keepalive():
    while True:
        time.sleep(10)
        with main.state_lock:
            for g in main.state["guns"].values():
                if g.get("alive"):
                    g["last_seen"] = time.time()
            snap = [dict(g) for g in main.state["guns"].values() if g.get("alive")]
        for g in snap:
            main.push_sse("gun_status", g)

threading.Thread(target=_keepalive, daemon=True).start()
main.log_event("DEMO MODE — seeded FDC + 4 guns (ไม่มีฮาร์ดแวร์)")

print(">> FDC demo running — open http://localhost:5000  (Ctrl-C to stop)")
main.app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)
