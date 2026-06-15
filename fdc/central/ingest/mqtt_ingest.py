"""
mqtt_ingest.py — สมาชิก MQTT ของศูนย์: subscribe ทุก ศอย. → เขียน DB → push realtime

topic ที่ ศอย. (uplink.py) ส่งขึ้นมา ภายใต้ base = fdc/<unit>/<node> :
    .../status              สถานะ online/LWT  {online, unit, node, ts|reason}
    .../fdc_gps             GPS ของ ศอย.
    .../gun/<id>/status     สถานะหมู่ปืน
    .../fire_mission        หลักฐานยิงที่คำนวณ/สั่ง
    .../event               เหตุการณ์/audit

ออกแบบให้ทนลิงก์หลุด: เชื่อมใหม่อัตโนมัติวนไปเรื่อยๆ ไม่ทำให้ service ตาย
"""
import ssl
import time
import json
import asyncio
import logging

import aiomqtt

import config
import db
import hub

logger = logging.getLogger("mqtt")


def _build_tls_context() -> ssl.SSLContext | None:
    if not config.MQTT_TLS:
        return None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = not config.MQTT_TLS_INSECURE
    ctx.load_verify_locations(cafile=config.MQTT_CA_CERT)
    ctx.load_cert_chain(certfile=config.MQTT_CLIENT_CERT,
                        keyfile=config.MQTT_CLIENT_KEY)
    if config.MQTT_TLS_INSECURE:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    return ctx


def _now() -> float:
    return time.time()


def _parse_topic(topic: str):
    """fdc/<unit>/<node>/<rest...> → (unit, node, [rest])  หรือ None ถ้าไม่ตรงรูป"""
    parts = topic.split("/")
    if len(parts) < 4 or parts[0] != "fdc":
        return None
    return parts[1], parts[2], parts[3:]


async def _handle(topic: str, payload: bytes):
    parsed = _parse_topic(topic)
    if not parsed:
        return
    unit, node, rest = parsed
    try:
        data = json.loads(payload.decode("utf-8"))
    except Exception:
        logger.warning(f"bad JSON on {topic}")
        return

    # timestamp: ใช้ของในข้อความถ้ามี ไม่งั้นเวลารับ
    ts = data.get("ts") if isinstance(data.get("ts"), (int, float)) else _now()
    kind = None

    if rest == ["status"]:
        kind = "node_status"
        online = bool(data.get("online"))
        await db.upsert_node_status(unit, node, online, ts,
                                    reason=data.get("reason"))
    elif rest == ["fdc_gps"]:
        kind = "fdc_gps"
        await db.insert_fdc_gps(unit, node, ts, data)
    elif len(rest) == 3 and rest[0] == "gun" and rest[2] == "status":
        kind = "gun_status"
        await db.insert_gun_status(unit, node, rest[1], ts, data)
    elif rest == ["fire_mission"]:
        kind = "fire_mission"
        await db.insert_fire_mission(unit, node, ts, data)
    elif rest == ["event"]:
        kind = "event"
        await db.insert_event(unit, node, ts, data)
    else:
        return    # topic ที่ไม่รู้จัก — ข้าม

    # push ขึ้น dashboard realtime
    await hub.broadcaster.publish({
        "kind": kind, "unit": unit, "node": node,
        "topic": topic, "data": data, "time": ts,
    })


async def run_forever():
    """ลูปหลักของ ingest — เชื่อม broker, subscribe, อ่านข้อความ; เชื่อมใหม่เมื่อหลุด"""
    tls = _build_tls_context()
    while True:
        try:
            logger.info(f"connecting MQTT {config.MQTT_HOST}:{config.MQTT_PORT}")
            async with aiomqtt.Client(
                hostname=config.MQTT_HOST,
                port=config.MQTT_PORT,
                identifier=config.MQTT_CLIENT_ID,
                keepalive=config.MQTT_KEEPALIVE,
                tls_context=tls,
            ) as client:
                hub.mqtt_client = client
                await client.subscribe(config.MQTT_TOPIC, qos=1)
                logger.info(f"subscribed {config.MQTT_TOPIC} — ingest live")
                async for msg in client.messages:
                    try:
                        await _handle(str(msg.topic), msg.payload)
                    except Exception as e:
                        logger.error(f"handle error on {msg.topic}: {e}")
        except aiomqtt.MqttError as e:
            logger.warning(f"MQTT disconnected: {e} — retry in 3s")
        except Exception as e:
            logger.error(f"ingest loop error: {e} — retry in 3s")
        finally:
            hub.mqtt_client = None
        await asyncio.sleep(3)
