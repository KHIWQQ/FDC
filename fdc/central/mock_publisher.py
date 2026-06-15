#!/usr/bin/env python3
"""
mock_publisher.py — จำลอง ศอย. ส่ง telemetry ขึ้น broker (สำหรับทดสอบ D1–D4)

ใช้ทดสอบศูนย์บัญชาการได้ครบโดย "ไม่ต้องมี gun unit / dongle จริง":
หมุดบนแผนที่จะขยับ, มีภารกิจยิงโผล่, AAR/รายงานมีข้อมูลให้ดู

ต้องมีใบรับรองของหน่วยนี้ (CN = "<unit>/<node>") จาก make-certs.sh
และ broker (mosquitto) รันอยู่ (docker compose up)

ตัวอย่าง (หลัง ./make-certs.sh BN1-A/FDC01):
  pip install paho-mqtt
  python3 mock_publisher.py --unit BN1-A --node FDC01 \
      --host localhost --port 8883 \
      --ca certs/ca.crt --cert certs/BN1-A_FDC01.crt --key certs/BN1-A_FDC01.key \
      --base-lat 13.736 --base-lon 100.523

หมายเหตุ: broker ตรวจ hostname จาก CN ของ broker cert — ถ้า broker.crt ออกด้วย
CN=localhost ให้ใช้ --host localhost (ไม่งั้น TLS verify จะ fail)
"""
import ssl
import json
import time
import math
import argparse

import paho.mqtt.client as mqtt


def now() -> float:
    return time.time()


def jitter(seed: int, scale: float) -> float:
    """ค่าหลอกแบบ deterministic (เลี่ยง random เพื่อให้ทำซ้ำได้)"""
    return math.sin(seed * 1.3) * scale


def main():
    ap = argparse.ArgumentParser(description="จำลอง ศอย. ส่ง telemetry (ทดสอบศูนย์)")
    ap.add_argument("--unit", required=True)
    ap.add_argument("--node", required=True)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--ca", required=True)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--base-lat", type=float, default=13.736)
    ap.add_argument("--base-lon", type=float, default=100.523)
    ap.add_argument("--guns", type=int, default=4)
    ap.add_argument("--interval", type=float, default=2.0, help="วินาทีต่อรอบ")
    ap.add_argument("--insecure", action="store_true", help="ข้ามตรวจ hostname (dev)")
    args = ap.parse_args()

    base = f"fdc/{args.unit}/{args.node}"

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_verify_locations(cafile=args.ca)
    ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
    if args.insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    cli = mqtt.Client(client_id=f"mock-{args.unit}-{args.node}", clean_session=False)
    cli.tls_set_context(ctx)
    # LWT เหมือน ศอย. จริง
    cli.will_set(f"{base}/status",
                 json.dumps({"online": False, "unit": args.unit, "node": args.node,
                             "reason": "lost"}), qos=1, retain=True)
    cli.connect(args.host, args.port, 30)
    cli.loop_start()

    def pub(sub, payload, retain=False):
        cli.publish(f"{base}/{sub}", json.dumps(payload, separators=(",", ":")),
                    qos=1, retain=retain)

    pub("status", {"online": True, "unit": args.unit, "node": args.node, "ts": now()},
        retain=True)
    pub("event", {"ts": now(), "level": "info", "msg": "MOCK ศอย. online"})
    print(f"✔ mock {args.unit}/{args.node} → {args.host}:{args.port} (Ctrl-C เพื่อหยุด)")

    i = 0
    try:
        while True:
            i += 1
            # GPS ศอย. — ดริฟต์เล็กน้อยรอบฐาน
            pub("fdc_gps", {
                "ts": now(), "lat": args.base_lat + jitter(i, 0.0008),
                "lon": args.base_lon + jitter(i + 5, 0.0008), "alt": 12.0,
                "fix": 1, "sats": 9, "bat": 100, "chg": True,
            })
            # หมู่ปืนแต่ละกระบอก กระจายรอบฐาน
            for g in range(1, args.guns + 1):
                pub(f"gun/{g}/status", {
                    "id": g, "lat": args.base_lat + 0.002 * g + jitter(i + g, 0.0004),
                    "lon": args.base_lon + 0.0015 * g + jitter(i + g * 2, 0.0004),
                    "az": (1200 + g * 100 + i) % 6400, "el": 800 + g * 20,
                    "hdg": (g * 90 + i) % 360, "bat": max(20, 100 - (i % 80)),
                    "rssi": -60 - g, "snr": 9.0,
                })
            # ภารกิจยิงเป็นระยะ ๆ (ทุก ~10 รอบ)
            if i % 10 == 0:
                gun = (i // 10) % args.guns + 1
                pub("fire_mission", {
                    "ts": now(), "gun": gun,
                    "tgt_lat": args.base_lat + 0.03 + jitter(i, 0.01),
                    "tgt_lon": args.base_lon + 0.03 + jitter(i + 3, 0.01),
                    "range_m": 4200 + (i % 30) * 50, "az_mils": (i * 13) % 6400,
                    "el_mils": 850 + (i % 200), "charge": 3 + (i % 3),
                    "drift_mils": 5, "source": "mock",
                })
                pub("event", {"ts": now(), "level": "info",
                              "msg": f"FIRE MISSION gun {gun} (mock)"})
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pub("status", {"online": False, "unit": args.unit, "node": args.node,
                       "reason": "shutdown"}, retain=True)
        cli.loop_stop()
        cli.disconnect()
        print("\nหยุดแล้ว")


if __name__ == "__main__":
    main()
