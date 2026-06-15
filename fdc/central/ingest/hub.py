"""
hub.py — สถานะร่วมระหว่าง ingest กับ API:
  - ตัวกระจายข้อความ realtime ไปยัง WebSocket ของ dashboard ผบ.
  - ตัวอ้างถึง MQTT client ที่เชื่อมอยู่ (ให้ API ใช้ publish คำสั่ง downlink)
"""
import asyncio
import logging

logger = logging.getLogger("hub")


class Broadcaster:
    """fan-out ข้อความ JSON ไปยังทุก WebSocket ที่ subscribe อยู่"""

    def __init__(self):
        self._subs: set[asyncio.Queue] = set()
        self._lock = asyncio.Lock()

    async def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=1000)
        async with self._lock:
            self._subs.add(q)
        return q

    async def unsubscribe(self, q: asyncio.Queue):
        async with self._lock:
            self._subs.discard(q)

    async def publish(self, message: dict):
        # ส่ง dict ดิบ — ให้แต่ละ WebSocket กรองตามขอบเขตผู้ใช้ (RBAC) ก่อน serialize
        async with self._lock:
            dead = []
            for q in self._subs:
                try:
                    q.put_nowait(message)
                except asyncio.QueueFull:
                    dead.append(q)        # ผู้รับช้าเกิน → ตัดทิ้ง กัน backpressure
            for q in dead:
                self._subs.discard(q)

    @property
    def count(self) -> int:
        return len(self._subs)


# ระดับโมดูล — แชร์ทั้งโปรเซส
broadcaster = Broadcaster()

# MQTT client ที่ ingest กำลังใช้ (None เมื่อยังไม่เชื่อม) — API ใช้ publish คำสั่ง
mqtt_client = None
