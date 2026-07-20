"""
demo.py — ตัวกำเนิดข้อมูลสาธิต (mock telemetry) ฝั่งศูนย์ (ปุ่ม DEMO)

สร้าง ศอย. จำลองหลายหน่วยกระจายทั่วประเทศ แล้วป้อน telemetry เข้า "เส้นทาง
เดียวกับของจริง" โดยเรียก mqtt_ingest._handle() ตรง ๆ — เขียน DB + push WebSocket
จึงเห็นหมุดขยับ / ภารกิจยิง / ฟีดสด บน dashboard ได้โดยไม่ต้องมี broker หรือ
ฮาร์ดแวร์ (ต่างจาก mock_publisher.py ที่ยิงผ่าน MQTT/mTLS จากภายนอก)

  POST /api/demo/start  → เริ่มป้อน          POST /api/demo/stop  → หยุด (หน่วย→offline)
  GET  /api/demo/status → สถานะ              POST /api/demo/clear → ลบข้อมูลสาธิต

ทุกหน่วยสาธิตใช้ unit ขึ้นต้นด้วย DEMO_PREFIX จึงล้างทิ้งได้แม่นยำโดยไม่แตะข้อมูลจริง
ปิดในระบบจริง: ตั้ง DEMO_ENABLED=false
"""
import time
import math
import json
import asyncio
import logging

import mqtt_ingest

logger = logging.getLogger("demo")

DEMO_PREFIX = "DEMO-"
INTERVAL = 2.0                 # วินาทีต่อรอบ (เท่ากับ mock_publisher)

# ศอย. จำลอง — กระจายทั่วประเทศเพื่อให้แผนที่ระดับประเทศมีหมุดครบทุกภาค
#   (unit, node, base_lat, base_lon, จำนวนกระบอก)
DEMO_UNITS = [
    ("DEMO-N",  "CNX", 18.79,  98.98, 4),   # เชียงใหม่ (เหนือ)
    ("DEMO-NE", "KKN", 16.43, 102.83, 3),   # ขอนแก่น (อีสาน)
    ("DEMO-NE", "UBN", 15.24, 104.85, 4),   # อุบลราชธานี (อีสานล่าง)
    ("DEMO-C",  "LOP", 14.80, 100.65, 6),   # ลพบุรี (กลาง — ศูนย์การทหารปืนใหญ่)
    ("DEMO-W",  "KAN", 14.02,  99.53, 3),   # กาญจนบุรี (ตะวันตก)
    ("DEMO-S",  "SKA",  7.01, 100.47, 4),   # สงขลา (ใต้)
]

_task: asyncio.Task | None = None
_state = {"running": False, "since": None, "ticks": 0, "units": len(DEMO_UNITS)}


def _jit(seed: int, scale: float) -> float:
    """ค่าหลอกแบบ deterministic (เลี่ยง random เพื่อให้ทำซ้ำได้ — เหมือน mock_publisher)"""
    return math.sin(seed * 1.3) * scale


async def _emit(unit: str, node: str, sub: str, payload: dict):
    """ป้อน 1 ข้อความเข้า ingest path เดียวกับ telemetry จริง (DB + WebSocket)"""
    topic = f"fdc/{unit}/{node}/{sub}"
    await mqtt_ingest._handle(topic, json.dumps(payload).encode("utf-8"))


async def _run():
    now = time.time
    # หน่วยทั้งหมดออนไลน์ + เหตุการณ์เปิดระบบ
    for unit, node, lat, lon, _g in DEMO_UNITS:
        await _emit(unit, node, "status",
                    {"online": True, "unit": unit, "node": node, "ts": now()})
        await _emit(unit, node, "event",
                    {"ts": now(), "level": "info",
                     "msg": f"DEMO {unit}/{node} ออนไลน์ (ข้อมูลสาธิต)"})
        # ปักเป้าเฝ้าตรวจ 1 จุด (ยังไม่ยิง) — โชว์ว่าเป้าขึ้น COP ตั้งแต่ปักหมุด
        await _emit(unit, node, f"target/{node}-T1", {
            "id": f"{node}-T1", "lat": lat + 0.06, "lon": lon + 0.04,
            "name": f"เป้าเฝ้าตรวจ {node}", "note": "สาธิต — ยังไม่สั่งยิง",
            "active": True, "ts": now()})
    i = 0
    try:
        while True:
            i += 1
            _state["ticks"] = i
            for idx, (unit, node, lat, lon, guns) in enumerate(DEMO_UNITS):
                # GPS ศอย. — ดริฟต์เล็กน้อยรอบฐาน, แบตไหลขึ้นลง
                await _emit(unit, node, "fdc_gps", {
                    "ts": now(), "lat": lat + _jit(i + idx, 0.0008),
                    "lon": lon + _jit(i + idx + 5, 0.0008), "alt": 30.0,
                    "fix": 1, "sats": 9, "bat": max(35, 100 - (i % 60)), "chg": True,
                })
                # หมู่ปืนกระจายรอบฐาน
                for g in range(1, guns + 1):
                    await _emit(unit, node, f"gun/{g}/status", {
                        "id": g, "lat": lat + 0.004 * g + _jit(i + g, 0.003),
                        "lon": lon + 0.003 * g + _jit(i + g * 2, 0.003),
                        "az": (1200 + g * 100 + i * 7) % 6400, "el": 800 + g * 20,
                        "hdg": (g * 90 + i * 3) % 360,
                        "bat": max(20, 100 - ((i + g * 5) % 80)),
                        "rssi": -60 - g, "snr": 9.0,
                    })
                # ขยับเป้าเฝ้าตรวจเล็กน้อยเป็นระยะ (โชว์ว่าเป้าอัปเดตสดบน COP)
                if (i + idx) % 15 == 0:
                    await _emit(unit, node, f"target/{node}-T1", {
                        "id": f"{node}-T1", "lat": lat + 0.06 + _jit(i, 0.01),
                        "lon": lon + 0.04 + _jit(i + 2, 0.01),
                        "name": f"เป้าเฝ้าตรวจ {node}", "note": "สาธิต — ยังไม่สั่งยิง",
                        "active": True, "ts": now()})
                # ภารกิจยิงเป็นระยะ — เหลื่อมเวลากันแต่ละหน่วย (ทุก ~16 วิ/หน่วย)
                if (i + idx * 3) % 8 == 0:
                    gun = ((i // 8) % guns) + 1
                    await _emit(unit, node, "fire_mission", {
                        "ts": now(), "gun": gun,
                        "tgt_lat": lat + 0.05 + _jit(i, 0.02),
                        "tgt_lon": lon + 0.05 + _jit(i + 3, 0.02),
                        "range_m": 4200 + (i % 40) * 60, "az_mils": (i * 13) % 6400,
                        "el_mils": 850 + (i % 200), "charge": 3 + (i % 4),
                        "drift_mils": 5, "source": "demo",
                    })
                    await _emit(unit, node, "event", {
                        "ts": now(), "level": "info",
                        "msg": f"ภารกิจยิง GUN {gun} (สาธิต)"})
            await asyncio.sleep(INTERVAL)
    except asyncio.CancelledError:
        # หยุด → ตั้งทุกหน่วยเป็น offline (เหมือน ศอย. หลุดลิงก์)
        for unit, node, _lat, _lon, _g in DEMO_UNITS:
            try:
                await _emit(unit, node, "status",
                            {"online": False, "unit": unit, "node": node,
                             "reason": "demo stopped"})
            except Exception:
                pass
        raise


def status() -> dict:
    return dict(_state)


async def start() -> bool:
    """เริ่ม task สาธิต — คืน True ถ้าเพิ่งเริ่ม, False ถ้ากำลังรันอยู่แล้ว"""
    global _task
    if _task and not _task.done():
        return False
    _state.update(running=True, since=time.time(), ticks=0, units=len(DEMO_UNITS))
    _task = asyncio.create_task(_run())
    logger.info("demo generator started (%d units)", len(DEMO_UNITS))
    return True


async def stop() -> bool:
    """หยุด task สาธิต — คืน True ถ้าหยุดสำเร็จ, False ถ้าไม่ได้รันอยู่"""
    global _task
    _state["running"] = False
    if not _task or _task.done():
        _task = None
        return False
    _task.cancel()
    try:
        await _task
    except asyncio.CancelledError:
        pass
    _task = None
    logger.info("demo generator stopped")
    return True
