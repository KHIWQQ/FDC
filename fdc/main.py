"""
main.py — Artillery FCS Web Application (Flask)

Endpoints:
  GET  /                      → Web UI
  GET  /api/state             → Current state (guns, fdc pos, targets)
  POST /api/fire_mission      → Compute + send fire command
  POST /api/ping/<gun_id>     → Ping a gun
  GET  /api/stream            → SSE stream for real-time updates
  POST /api/target            → Add/update target marker
  DELETE /api/target/<id>     → Remove target
"""

import os
import sys
import json
import time
import threading
import logging
import webbrowser
import struct
import zlib
import urllib.request
from flask import Flask, request, jsonify, render_template, Response, stream_with_context

from fdc_radio  import FdcRadio
from ballistic  import solve_fire_mission
from uplink     import FdcUplink

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s")
logger = logging.getLogger("main")


def load_config() -> dict:
    """Load config.yaml if present. Missing file/pyyaml → {} (edge-autonomous standalone)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.yaml")
    if getattr(sys, "frozen", False):
        path = os.path.join(sys._MEIPASS, "config.yaml")
    try:
        import yaml
        with open(path, "r", encoding="utf-8") as f:
            return yaml.safe_load(f) or {}
    except FileNotFoundError:
        logger.info("config.yaml not found — running standalone (uplink disabled)")
    except ImportError:
        logger.info("pyyaml not installed — running standalone (uplink disabled)")
    except Exception as e:
        logger.error(f"config.yaml load failed: {e} — running standalone")
    return {}

# When packaged with PyInstaller (--onefile), templates live under sys._MEIPASS.
if getattr(sys, "frozen", False):
    _base = sys._MEIPASS
    app = Flask(__name__,
                template_folder=os.path.join(_base, "templates"),
                static_folder=os.path.join(_base, "static"))
else:
    app = Flask(__name__)

CONFIG = load_config()
BOOT_TIME = time.time()   # server start epoch → ใช้คำนวณ uptime ฝั่ง UI
# radio.psk (hex32) ใน config.yaml → เปิด challenge-response ยืนยัน dongle ตัวจริง
radio  = FdcRadio(psk=(CONFIG.get("radio") or {}).get("psk") or None)
# จังหวะ heartbeat (วินาที) — เว็บ broadcast ping ทุกเท่านี้ ให้ตัวชี้ LINK ที่หมู่ปืนสด (0 = ปิด)
HEARTBEAT_SEC = (CONFIG.get("radio") or {}).get("heartbeat_sec", 10)
uplink = FdcUplink(CONFIG)

# ============================================================
#  Shared state (protected by state_lock)
# ============================================================
state_lock = threading.Lock()
state = {
    "radio": {
        "connected": False, "port": None, "node": None, "status": "init"
    },
    "uplink": {
        "enabled": False, "connected": False, "queued": 0,
        "unit": None, "node": None
    },
    "fdc": {
        "lat": 0.0, "lon": 0.0, "alt": 0.0,
        "fix": 0,   "sats": 0,  "valid": False
    },
    "guns": {
        str(i): {
            "id": i, "alive": False,
            "lat": 0.0, "lon": 0.0,
            "az": 0, "el": 0,
            "hdg": None,
            "bat": 0, "rssi": 0, "snr": 0.0,
            "role": "primary" if i == 1 else "backup",   # ที่ตั้งหลัก 1 / สำรอง 3
            "last_seen": None
        } for i in range(1, 5)
    },
    "targets": {},          # id → {lat, lon, name, note}
    "firing_pos": {         # ที่ตั้งยิงบนแผนที่ — หลัก 1 + สำรอง 3
        "primary": None, "backup": [None, None, None]
    },
    "last_fire": None,      # last fire solution
    "log": []               # last 50 events
}

# SSE subscribers
sse_clients = []
sse_lock    = threading.Lock()


# ============================================================
#  State helpers
# ============================================================

def log_event(msg: str, level: str = "info"):
    entry = {"t": time.strftime("%H:%M:%S"), "msg": msg, "level": level}
    with state_lock:
        state["log"].append(entry)
        if len(state["log"]) > 50:
            state["log"].pop(0)
    push_sse("log", entry)
    uplink.publish("event", entry)


def push_sse(event: str, data: dict):
    payload = f"event: {event}\ndata: {json.dumps(data)}\n\n"
    with sse_lock:
        dead = []
        for q in sse_clients:
            try:
                q.put_nowait(payload)
            except Exception:
                dead.append(q)
        for q in dead:
            sse_clients.remove(q)


def on_radio_status(event: str, data: dict):
    """Called by FdcRadio on connect/disconnect/searching → mirror to UI."""
    if event == "connected":
        radio_state = {"connected": True, "port": data.get("port"),
                       "node": data.get("node"), "freq": data.get("freq"),
                       "fw": data.get("fw"), "status": "connected"}
    elif event == "searching":
        radio_state = {"connected": False, "port": None, "node": None, "status": "searching"}
    else:  # disconnected
        radio_state = {"connected": False, "port": None, "node": None, "status": "disconnected"}

    with state_lock:
        state["radio"] = radio_state
    push_sse("radio_status", radio_state)

    if event == "connected":
        log_event(f"RADIO connected: {data.get('port')} (node {data.get('node')})")
    elif event == "disconnected":
        log_event(f"RADIO disconnected ({data.get('reason', '')})", "error")
    elif event == "searching":
        log_event("RADIO searching for FDC dongle…")


def on_uplink_status(connected: bool):
    """Mirror uplink (MQTT) connectivity to the UI."""
    with state_lock:
        state["uplink"].update(uplink.status())
    push_sse("uplink_status", state["uplink"])
    log_event(f"UPLINK {'connected to central' if connected else 'disconnected'}",
              "info" if connected else "error")


def on_central_command(cmd_type: str, data: dict):
    """Handle an authenticated command pushed down from central command.

    Signature/anti-replay already verified inside FdcUplink before we get here.
    """
    if cmd_type == "fire_mission":
        result, code = execute_fire_mission(
            tgt_lat=data.get("tgt_lat"),
            tgt_lon=data.get("tgt_lon"),
            gun_id=data.get("gun", "1"),
            charge=data.get("charge"),
            source="central",
        )
        if code != 200:
            log_event(f"Central fire mission rejected: {result.get('error')}", "error")
    elif cmd_type == "ping":
        radio.ping_gun(int(data.get("gun", 1)))
    elif cmd_type == "rescan":
        radio.rescan()
    else:
        log_event(f"Unknown central command: {cmd_type}", "error")


# ============================================================
#  Radio poller thread
# ============================================================

def radio_poller():
    import queue as q_mod
    while True:
        msgs = radio.get_messages()
        for msg in msgs:
            mtype = msg.get("type")

            if mtype == "FDC_GPS":
                with state_lock:
                    state["fdc"].update({
                        "lat":   msg.get("lat", 0),
                        "lon":   msg.get("lon", 0),
                        "alt":   msg.get("alt", 0),
                        "fix":   msg.get("fix", 0),
                        "sats":  msg.get("sats", 0),
                        "valid": msg.get("fix", 0) >= 2
                    })
                    fdc_snap = dict(state["fdc"])   # snapshot ใต้ lock กัน read แบบ torn
                push_sse("fdc_gps", fdc_snap)
                uplink.publish("fdc_gps", fdc_snap)

            elif mtype == "GUN_STATUS":
                gun_id = str(msg.get("gun", 0))
                if gun_id in state["guns"]:
                    with state_lock:
                        state["guns"][gun_id].update({
                            "alive":     True,
                            "lat":       msg.get("lat", 0),
                            "lon":       msg.get("lon", 0),
                            "az":        msg.get("az", 0),
                            "el":        msg.get("el", 0),
                            "hdg":       msg.get("hdg", None),
                            "bat":       msg.get("bat", 0),
                            "rssi":      msg.get("rssi", 0),
                            "snr":       msg.get("snr", 0),
                            "last_seen": time.time()
                        })
                        gun_snap = dict(state["guns"][gun_id])   # snapshot ใต้ lock
                    push_sse("gun_status", gun_snap)
                    uplink.publish(f"gun/{gun_id}/status", gun_snap)

            elif mtype == "GUN_ACK":
                log_event(f"Gun {msg.get('gun')} ACK seq={msg.get('seq')} ok={msg.get('ok')}")
                push_sse("gun_ack", msg)

            elif mtype == "PONG":
                log_event(f"Gun {msg.get('gun')} PONG rssi={msg.get('rssi')} snr={msg.get('snr')}")
                push_sse("pong", msg)

            elif mtype == "ERROR":
                log_event(f"Dongle error: {msg.get('msg')}", "error")

        time.sleep(0.1)


# ============================================================
#  Routes
# ============================================================

@app.route("/")
def index():
    return render_template("index.html")


# ============================================================
#  Offline map tiles — caching proxy (/tiles/<layer>/z/x/y.png)
#  ออนไลน์ : ดึงจาก upstream + เซฟลงแคช   |  ออฟไลน์ : เสิร์ฟจากแคช
#  ไม่มีในแคช + ออฟไลน์ : ไทล์เทาเปล่า (แผนที่ไม่พัง)
#  → เปิดดูพื้นที่ปฏิบัติการตอนมีเน็ตที่ฐาน 1 ครั้ง แล้วออกสนามใช้แบบไม่มีเน็ตได้
# ============================================================

# แคชต้องเขียนได้ — อย่าใช้ _MEIPASS (read-only ตอนแพ็กเป็น .exe)
if getattr(sys, "frozen", False):
    _tile_base = os.path.dirname(sys.executable)
else:
    _tile_base = os.path.dirname(os.path.abspath(__file__))
TILE_CACHE_DIR = os.path.join(_tile_base, "tile_cache")

# layer → upstream URL — ใช้ Esri ArcGIS ทั้งหมด (ลำดับ z/y/x) เพราะเข้าถึงได้เสถียร
# ไม่ผูกกับ OSM/OpenTopoMap ที่บล็อกการใช้ผ่าน proxy/ปริมาณมาก (คืน 403)
TILE_UPSTREAM = {
    "street": "https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}",
    "sat":    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    "topo":   "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}",
}
TILE_UA = "FDC-FCS/1.0 (artillery fire direction center; offline field tile cache)"


def _solid_png(w: int, h: int, rgb: tuple) -> bytes:
    """PNG สีเดียว (stdlib ล้วน) — ใช้เป็นไทล์ placeholder ตอนออฟไลน์+ไม่มีในแคช."""
    def _chunk(typ: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    sig  = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)      # 8-bit truecolour RGB
    raw  = (b"\x00" + bytes(rgb) * w) * h                    # filter byte 0 + pixels/แถว
    return sig + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(raw, 9)) + _chunk(b"IEND", b"")


PLACEHOLDER_TILE = _solid_png(256, 256, (40, 46, 40))        # เทาเข้ม = ยังไม่มีในแคช


def _img_ctype(data: bytes) -> str:
    if data[:3] == b"\xff\xd8\xff":       return "image/jpeg"   # Esri = JPEG
    if data[:8] == b"\x89PNG\r\n\x1a\n":  return "image/png"
    return "image/png"


def _tile_resp(data: bytes, status: str, cache: bool = True) -> Response:
    hdrs = {"X-Tile-Cache": status}
    if cache:
        hdrs["Cache-Control"] = "public, max-age=604800"
    return Response(data, mimetype=_img_ctype(data), headers=hdrs)


@app.route("/tiles/<layer>/<int:z>/<int:x>/<int:y>.png")
def tile_proxy(layer, z, x, y):
    tmpl = TILE_UPSTREAM.get(layer)
    if tmpl is None:
        return _tile_resp(PLACEHOLDER_TILE, "bad-layer", cache=False)
    if not (0 <= z <= 22 and 0 <= x < (1 << z) and 0 <= y < (1 << z)):
        return _tile_resp(PLACEHOLDER_TILE, "oob", cache=False)

    path = os.path.join(TILE_CACHE_DIR, layer, str(z), str(x), f"{y}.png")

    # 1) มีในแคช → เสิร์ฟทันที (เส้นทางออฟไลน์)
    if os.path.isfile(path):
        try:
            with open(path, "rb") as f:
                return _tile_resp(f.read(), "hit")
        except OSError:
            pass  # อ่านไม่ได้ → ลองดึงใหม่

    # 2) ไม่มีในแคช → ดึงจาก upstream (ต้องมีเน็ต) แล้วเซฟ
    try:
        req = urllib.request.Request(tmpl.format(z=z, x=x, y=y),
                                     headers={"User-Agent": TILE_UA})
        with urllib.request.urlopen(req, timeout=8) as r:
            data  = r.read()
            ctype = r.headers.get("Content-Type", "")
        # ต้องเป็น "รูปจริง" เท่านั้นถึงแคช — กันหน้า block/HTML ที่ตอบ 200 มาปนเป็นไทล์
        if not ctype.startswith("image/") or len(data) < 100:
            return _tile_resp(PLACEHOLDER_TILE, "bad-upstream", cache=False)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = f"{path}.{threading.get_ident()}.tmp"          # atomic write กัน partial read
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
        return _tile_resp(data, "miss-fetched")
    except Exception:
        # 3) ออฟไลน์/upstream ล่ม + ไม่มีในแคช → ไทล์เปล่า
        return _tile_resp(PLACEHOLDER_TILE, "offline-miss", cache=False)


@app.route("/api/state")
def api_state():
    with state_lock:
        return jsonify(dict(state, boot_time=BOOT_TIME))


def _to_float(v):
    """Parse to float, or None if blank/invalid (for optional altitude fields)."""
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def execute_fire_mission(tgt_lat, tgt_lon, gun_id, charge=None, alt_diff_m=0.0,
                         send=True, source="local", order="FIRE",
                         gun_lat_override=None, gun_lon_override=None):
    """Compute a fire solution; transmit to the gun only when send=True.

    send=False → CALCULATE: คำนวณ/พรีวิวเท่านั้น (ไม่ส่งวิทยุ ไม่บันทึก/ไม่ broadcast)
    send=True  → FIRE: ส่งคำสั่งถึงปืนจริง + log + push SSE/uplink
    alt_diff_m = target altitude − gun altitude (m); 0 = no angle-of-site correction.

    Returns (result_dict, http_status).
    """
    gun_id = str(gun_id)
    if tgt_lat is None or tgt_lon is None:
        return {"error": "tgt_lat and tgt_lon required"}, 400

    with state_lock:
        gun = dict(state["guns"].get(gun_id) or {})   # copy ใต้ lock กัน lat/lon ถูกแก้กลางคัน
        fdc = dict(state["fdc"])

    if not gun:
        return {"error": f"Gun {gun_id} not found"}, 404

    # ตำแหน่งปืนที่ใช้คำนวณ: ปลายทางที่วางแผน (override) > GPS ปืนจริง > FDC (สำรอง)
    if gun_lat_override is not None and gun_lon_override is not None:
        gun_lat, gun_lon = gun_lat_override, gun_lon_override
        base = "planned"   # เสมือนปืนไปอยู่ที่ตั้งปลายทางแล้ว (เตรียมค่ายิงก่อนย้ายจริง)
    elif gun["lat"] != 0 and gun["lon"] != 0:
        gun_lat, gun_lon = gun["lat"], gun["lon"]
        base = "current"   # ตำแหน่งปืนปัจจุบัน
    elif fdc["valid"]:
        gun_lat, gun_lon = fdc["lat"], fdc["lon"]
        base = "fdc"
    else:
        return {"error": "No gun or FDC GPS fix"}, 400

    fdc_lat = fdc["lat"] if fdc["valid"] else gun_lat
    fdc_lon = fdc["lon"] if fdc["valid"] else gun_lon

    solution = solve_fire_mission(
        fdc_lat=fdc_lat, fdc_lon=fdc_lon,
        gun_lat=gun_lat, gun_lon=gun_lon,
        tgt_lat=tgt_lat, tgt_lon=tgt_lon,
        charge=charge,
        alt_diff_m=alt_diff_m,
    )

    if solution.get("error"):
        return {"error": solution["error"]}, 422

    az = solution["az_mils_corrected"] or solution["az_mils"]
    el = solution["el_mils"]
    chg = solution["charge"]
    solution["gun_id"] = int(gun_id)
    solution["source"] = source        # 'local' (เว็บ ศอย.) หรือ 'central' (สั่งจากศูนย์)
    solution["order"]  = order         # 'FIRE' = ยิงทันที | 'STANDBY' = เล็ง/รอสั่งยิง
    solution["base"]   = base          # 'planned' = เสมือนอยู่ปลายทางแผนย้าย | 'current' = ปืนปัจจุบัน | 'fdc'

    # CALCULATE — คำนวณอย่างเดียว ยังไม่ส่งถึงปืน ไม่บันทึก/ไม่ broadcast
    if not send:
        solution["sent"] = False
        return solution, 200

    # FIRE/STANDBY — ส่งคำสั่งถึงปืนจริงผ่านวิทยุ แล้วบันทึก + แจ้งเรียลไทม์/อัปลิงก์
    sent = radio.send_fire_cmd(int(gun_id), az, el, chg, order=order)
    solution["sent"] = sent

    with state_lock:
        state["last_fire"] = solution

    verb = "ยิง (FIRE)" if order == "FIRE" else "เล็ง/รอสั่งยิง (STANDBY)"
    log_event(
        f"Fire mission [{source}] {verb} → Gun {gun_id}: AZ={az} EL={el} CHG={chg} "
        f"Range={solution['range_m']}m", "fire"
    )
    push_sse("fire_mission", solution)
    uplink.publish("fire_mission", solution)

    return solution, 200


@app.route("/api/fire_mission", methods=["POST"])
def api_fire_mission():
    body = request.get_json(force=True)
    # ความสูงเป้า/ปืน (optional) → ผลต่างสำหรับ angle of site; ขาดค่าใดค่าหนึ่ง = ไม่ชดเชย
    tgt_alt = _to_float(body.get("tgt_alt"))
    gun_alt = _to_float(body.get("gun_alt"))
    alt_diff = (tgt_alt - gun_alt) if (tgt_alt is not None and gun_alt is not None) else 0.0
    result, code = execute_fire_mission(
        tgt_lat=body.get("tgt_lat"),
        tgt_lon=body.get("tgt_lon"),
        gun_id=body.get("gun", "1"),
        charge=body.get("charge"),     # None = auto
        alt_diff_m=alt_diff,
        send=bool(body.get("send", True)),   # CALCULATE ส่ง send:false → คำนวณเฉยๆ
        order=str(body.get("order", "FIRE")).upper(),  # FIRE = ยิง | STANDBY = เล็ง/รอยิง
        gun_lat_override=_to_float(body.get("gun_lat")),  # ระบุ = คำนวณเสมือนปืนอยู่พิกัดนี้ (แผนย้าย)
        gun_lon_override=_to_float(body.get("gun_lon")),
        source="local",
    )
    return jsonify(result), code


@app.route("/api/rescan", methods=["POST"])
def api_rescan():
    """Force the radio to re-run plug-and-play auto-detect."""
    radio.rescan()
    return jsonify({"ok": True})


@app.route("/api/ping/<int:gun_id>", methods=["POST"])
def api_ping(gun_id):
    ok = radio.ping_gun(gun_id)
    return jsonify({"sent": ok, "gun": gun_id})


@app.route("/api/gun/<int:gun_id>/role", methods=["POST"])
def api_gun_role(gun_id):
    """ตั้งบทบาทที่ตั้งยิงของปืน: 'primary' (หลัก — ได้กระบอกเดียว) หรือ 'backup' (สำรอง)."""
    body = request.get_json(force=True) or {}
    role = str(body.get("role", "primary")).lower()
    gid = str(gun_id)
    with state_lock:
        if gid not in state["guns"]:
            return jsonify({"error": f"Gun {gun_id} not found"}), 404
        if role == "primary":
            # มีปืนหลักได้กระบอกเดียว — ที่เหลือกลายเป็นสำรองทั้งหมด
            for k, g in state["guns"].items():
                g["role"] = "primary" if k == gid else "backup"
        else:
            state["guns"][gid]["role"] = "backup"
        affected = [dict(v) for v in state["guns"].values()]
    for g in affected:
        push_sse("gun_status", g)
    log_event(f"Gun {gun_id} → {'ที่ตั้งหลัก' if role == 'primary' else 'ที่ตั้งสำรอง'}")
    return jsonify({"ok": True, "gun": gun_id, "role": role})


@app.route("/api/firing_pos", methods=["POST"])
def api_firing_pos():
    """ตั้ง/อัปเดตที่ตั้งยิงบนแผนที่: primary={lat,lon} และ/หรือ backup=[{lat,lon} ×3]."""
    body = request.get_json(force=True) or {}
    with state_lock:
        fp = state["firing_pos"]
        if "primary" in body:
            fp["primary"] = body["primary"]
        if "backup" in body:
            bk = list(body["backup"] or [])
            fp["backup"] = (bk + [None, None, None])[:3]
        snapshot = {"primary": fp["primary"], "backup": list(fp["backup"])}
    push_sse("firing_pos", snapshot)
    return jsonify({"ok": True, "firing_pos": snapshot})


@app.route("/api/target", methods=["POST"])
def api_add_target():
    body = request.get_json(force=True)
    tgt_id = str(body.get("id", int(time.time())))
    with state_lock:
        state["targets"][tgt_id] = {
            "id":   tgt_id,
            "lat":  body.get("lat", 0),
            "lon":  body.get("lon", 0),
            "name": body.get("name", f"TGT-{tgt_id}"),
            "note": body.get("note", "")
        }
    push_sse("target_update", state["targets"][tgt_id])
    # แชร์เป้าขึ้น COP ระดับประเทศ "ตั้งแต่ปักหมุด" (ก่อนยิง) — retained = state ปัจจุบัน
    uplink.publish(f"target/{tgt_id}",
                   {**state["targets"][tgt_id], "active": True, "ts": time.time()},
                   retain=True)
    return jsonify({"ok": True, "id": tgt_id})


@app.route("/api/target/<tgt_id>", methods=["DELETE"])
def api_del_target(tgt_id):
    with state_lock:
        removed = state["targets"].pop(tgt_id, None)
    if removed:
        push_sse("target_remove", {"id": tgt_id})
        # ลบเป้าบน COP ด้วย (tombstone retained — กวาด retained เดิมที่ broker)
        uplink.publish(f"target/{tgt_id}",
                       {"id": tgt_id, "active": False, "ts": time.time()},
                       retain=True)
    return jsonify({"ok": removed is not None})


@app.route("/api/stream")
def api_stream():
    import queue
    q = queue.Queue(maxsize=100)
    with sse_lock:
        sse_clients.append(q)

    def generate():
        # Send current state immediately
        with state_lock:
            init_state = dict(state, boot_time=BOOT_TIME)
        yield f"event: init\ndata: {json.dumps(init_state)}\n\n"
        try:
            while True:
                try:
                    msg = q.get(timeout=20)
                    yield msg
                except Exception:
                    yield ": keepalive\n\n"
        except GeneratorExit:
            with sse_lock:
                if q in sse_clients:
                    sse_clients.remove(q)

    return Response(
        stream_with_context(generate()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"}
    )


# ============================================================
#  Link heartbeat — auto-ping ให้ตัวชี้ LINK ที่หมู่ปืนเป็น heartbeat สด
# ============================================================

def heartbeat_loop():
    """ทุก HEARTBEAT_SEC วินาที ส่ง broadcast ping 1 ครั้ง (ถึงทุกกระบอก).
    หมู่ปืน stamp lastFdcRx ทุกแพ็กเก็ต FDC ที่ผ่าน MAC+CRC → 'FDC: <กี่วิ>'
    บนจอกลายเป็นนาฬิกาเต้นจริง. ใช้ broadcast เพราะ SF12 airtime สูง
    (ping ทีละกระบอก + รอ pong จะกินอากาศเกินงบเมื่อมีหลายกระบอก)."""
    while True:
        time.sleep(HEARTBEAT_SEC)
        if radio.connected:
            radio.ping_broadcast()


# ============================================================
#  Entry point
# ============================================================

def open_browser_later(url: str, delay: float = 1.5):
    """Open the default browser once the server is up (one-click UX)."""
    if os.environ.get("FDC_NO_BROWSER"):
        return
    def _open():
        time.sleep(delay)
        try:
            webbrowser.open(url)
        except Exception as e:
            logger.debug(f"webbrowser.open failed: {e}")
    threading.Thread(target=_open, daemon=True).start()


if __name__ == "__main__":
    radio.status_callback = on_radio_status
    radio.start()

    uplink.status_callback  = on_uplink_status
    uplink.command_callback = on_central_command
    uplink.start()
    with state_lock:
        state["uplink"].update(uplink.status())

    log_event("FDC Web App started")

    poller = threading.Thread(target=radio_poller, daemon=True)
    poller.start()

    if HEARTBEAT_SEC and HEARTBEAT_SEC > 0:
        threading.Thread(target=heartbeat_loop, daemon=True).start()
        log_event(f"Link heartbeat: broadcast ping ทุก {HEARTBEAT_SEC}s")

    open_browser_later("http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)