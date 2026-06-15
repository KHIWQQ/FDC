#!/usr/bin/env python3
"""
sign_command.py — ตัวสร้าง/ส่งคำสั่ง downlink จากศูนย์บัญชาการ → ศอย.

นี่คือ "คู่แท้" ของ FdcUplink._verify_command() ในฝั่ง ศอย.
ผลิตซองคำสั่งที่ผ่านการตรวจ HMAC + anti-replay:
    {"type","seq","ts","data","sig"}

canonical string ที่เซ็น (ต้องตรงเป๊ะกับฝั่ง ศอย.):
    "{type}|{seq}|{ts}|{compact-sorted-json(data)}"

ใช้งาน:
  # เซ็นอย่างเดียว (พิมพ์ JSON ออกมา เอาไปส่งเองก็ได้)
  FDC_CMD_HMAC_KEY=<hex> ./sign_command.py --unit BN1-A --node FDC01 \
        fire_mission --tgt-lat 13.74 --tgt-lon 100.53 --gun 2

  # เซ็น + ส่งขึ้น broker ผ่าน mTLS เลย (ต้องมี paho-mqtt + ใบรับรอง)
  FDC_CMD_HMAC_KEY=<hex> ./sign_command.py --unit BN1-A --node FDC01 \
        --publish --broker central.example.mil --port 8883 \
        --ca ../certs/ca.crt --cert ../certs/broker.crt --key ../certs/broker.key \
        fire_mission --tgt-lat 13.74 --tgt-lon 100.53 --gun 2

seq เก็บใน .cmd_seq (ต่อ node) ให้เพิ่มขึ้นเสมอ — กัน replay ฝั่งรับ
"""

import os
import ssl
import sys
import json
import time
import hmac
import hashlib
import argparse


def canonical(cmd_type: str, seq: int, ts: int, data: dict) -> str:
    return "{}|{}|{}|{}".format(
        cmd_type, seq, ts,
        json.dumps(data, sort_keys=True, separators=(",", ":")))


def sign_envelope(key: bytes, cmd_type: str, seq: int, ts: int, data: dict) -> dict:
    sig = hmac.new(key, canonical(cmd_type, seq, ts, data).encode("utf-8"),
                   hashlib.sha256).hexdigest()
    return {"type": cmd_type, "seq": seq, "ts": ts, "data": data, "sig": sig}


def next_seq(node: str) -> int:
    """Monotonic per-node sequence, persisted next to this script."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f".cmd_seq_{node}")
    seq = 0
    if os.path.isfile(path):
        try:
            seq = int(open(path).read().strip())
        except Exception:
            seq = 0
    seq += 1
    with open(path, "w") as f:
        f.write(str(seq))
    return seq


def load_key() -> bytes:
    hexkey = os.environ.get("FDC_CMD_HMAC_KEY", "")
    if not hexkey:
        sys.exit("ERROR: ตั้ง env FDC_CMD_HMAC_KEY=<hex> ก่อน (คีย์เดียวกับฝั่ง ศอย.)")
    try:
        return bytes.fromhex(hexkey)
    except ValueError:
        sys.exit("ERROR: FDC_CMD_HMAC_KEY ต้องเป็น hex")


def publish(env: dict, topic: str, args):
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        sys.exit("ERROR: ต้อง pip install paho-mqtt เพื่อใช้ --publish")
    for label, p in (("ca", args.ca), ("cert", args.cert), ("key", args.key)):
        if not p or not os.path.isfile(p):
            sys.exit(f"ERROR: ต้องมีไฟล์ {label} (mTLS) — ขาด: {p}")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = True
    ctx.load_verify_locations(cafile=args.ca)
    ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)

    cli = mqtt.Client(client_id=f"central-signer")
    cli.tls_set_context(ctx)
    cli.connect(args.broker, args.port, 20)
    cli.loop_start()
    info = cli.publish(topic, json.dumps(env, separators=(",", ":")), qos=1)
    info.wait_for_publish(timeout=10)
    cli.loop_stop()
    cli.disconnect()
    print(f"✔ ส่งแล้ว → {topic}")


def main():
    ap = argparse.ArgumentParser(description="ลงนาม/ส่งคำสั่ง downlink ศูนย์ → ศอย.")
    ap.add_argument("--unit", required=True)
    ap.add_argument("--node", required=True)
    ap.add_argument("--publish", action="store_true", help="ส่งขึ้น broker เลย (mTLS)")
    ap.add_argument("--broker"); ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--ca"); ap.add_argument("--cert"); ap.add_argument("--key")

    sub = ap.add_subparsers(dest="cmd", required=True)
    fm = sub.add_parser("fire_mission", help="สั่งภารกิจยิง")
    fm.add_argument("--tgt-lat", type=float, required=True)
    fm.add_argument("--tgt-lon", type=float, required=True)
    fm.add_argument("--gun", default="1")
    fm.add_argument("--charge", type=int, default=None)
    pg = sub.add_parser("ping", help="ปิงหมู่ปืน"); pg.add_argument("--gun", default="1")
    sub.add_parser("rescan", help="สั่ง ศอย. สแกน dongle ใหม่")

    args = ap.parse_args()
    key = load_key()

    if args.cmd == "fire_mission":
        data = {"tgt_lat": args.tgt_lat, "tgt_lon": args.tgt_lon, "gun": args.gun}
        if args.charge is not None:
            data["charge"] = args.charge
    elif args.cmd == "ping":
        data = {"gun": args.gun}
    else:
        data = {}

    seq = next_seq(args.node)
    ts = int(time.time())
    env = sign_envelope(key, args.cmd, seq, ts, data)

    print(json.dumps(env, ensure_ascii=False, indent=2))

    if args.publish:
        if not args.broker:
            sys.exit("ERROR: --publish ต้องระบุ --broker")
        topic = f"cmd/fdc/{args.unit}/{args.node}/{args.cmd}"
        publish(env, topic, args)


if __name__ == "__main__":
    main()
