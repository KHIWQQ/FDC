"""
command.py — เซ็น + เผยแพร่คำสั่ง downlink (ศูนย์ → ศอย.)

เป็น "คู่แท้" ของ FdcUplink._verify_command() ฝั่ง ศอย. และใช้ canonical/รูปซอง
เดียวกับ central/sign_command.py (CLI) — ต่างกันแค่ออก seq จาก DB แทนไฟล์ และ
publish ผ่าน client เดิมของ ingest (ไม่เปิด TLS ใหม่ทุกครั้ง)

ซองที่เซ็น: {"type","seq","ts","data","sig"}
canonical:  "{type}|{seq}|{ts}|{compact-sorted-json(data)}"
topic:      cmd/fdc/<unit>/<node>/<type>
"""
import json
import time
import hmac
import hashlib

import config
import db


def _canonical(cmd_type: str, seq: int, ts: int, data: dict) -> str:
    return "{}|{}|{}|{}".format(
        cmd_type, seq, ts,
        json.dumps(data, sort_keys=True, separators=(",", ":")))


async def build_signed(unit: str, node: str, cmd_type: str, data: dict) -> dict:
    """สร้างซองคำสั่งที่เซ็นแล้ว (seq ใหม่ atomic จาก DB)."""
    key = config.cmd_key_for(unit)
    if key is None:
        raise PermissionError(
            f"ไม่มีคีย์ HMAC สำหรับหน่วย {unit} "
            f"(ตั้ง FDC_CMD_HMAC_KEYS หรือ FDC_CMD_HMAC_KEY)")
    seq = await db.next_cmd_seq(unit, node)
    ts = int(time.time())
    sig = hmac.new(key, _canonical(cmd_type, seq, ts, data).encode("utf-8"),
                   hashlib.sha256).hexdigest()
    return {"type": cmd_type, "seq": seq, "ts": ts, "data": data, "sig": sig}


def topic_for(unit: str, node: str, cmd_type: str) -> str:
    return f"cmd/fdc/{unit}/{node}/{cmd_type}"
