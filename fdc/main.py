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
            "last_seen": None
        } for i in range(1, 5)
    },
    "targets": {},          # id → {lat, lon, name, note}
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
                push_sse("fdc_gps", state["fdc"])
                uplink.publish("fdc_gps", state["fdc"])

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
                    push_sse("gun_status", state["guns"][gun_id])
                    uplink.publish(f"gun/{gun_id}/status", state["guns"][gun_id])

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


@app.route("/api/state")
def api_state():
    with state_lock:
        return jsonify(dict(state, boot_time=BOOT_TIME))


def execute_fire_mission(tgt_lat, tgt_lon, gun_id, charge=None, source="local"):
    """Compute + send a fire mission. Shared by the web route and MQTT downlink.

    Returns (result_dict, http_status).
    """
    gun_id = str(gun_id)
    if tgt_lat is None or tgt_lon is None:
        return {"error": "tgt_lat and tgt_lon required"}, 400

    with state_lock:
        gun = state["guns"].get(gun_id)
        fdc = dict(state["fdc"])

    if not gun:
        return {"error": f"Gun {gun_id} not found"}, 404

    # Use gun position if available, else FDC as fallback
    if gun["lat"] != 0 and gun["lon"] != 0:
        gun_lat, gun_lon = gun["lat"], gun["lon"]
    elif fdc["valid"]:
        gun_lat, gun_lon = fdc["lat"], fdc["lon"]
    else:
        return {"error": "No gun or FDC GPS fix"}, 400

    fdc_lat = fdc["lat"] if fdc["valid"] else gun_lat
    fdc_lon = fdc["lon"] if fdc["valid"] else gun_lon

    solution = solve_fire_mission(
        fdc_lat=fdc_lat, fdc_lon=fdc_lon,
        gun_lat=gun_lat, gun_lon=gun_lon,
        tgt_lat=tgt_lat, tgt_lon=tgt_lon,
        charge=charge
    )

    if solution.get("error"):
        return {"error": solution["error"]}, 422

    # Send to dongle
    az = solution["az_mils_corrected"] or solution["az_mils"]
    el = solution["el_mils"]
    chg = solution["charge"]

    sent = radio.send_fire_cmd(int(gun_id), az, el, chg)

    solution["gun_id"] = int(gun_id)
    solution["sent"]   = sent
    solution["source"] = source        # 'local' (เว็บ ศอย.) หรือ 'central' (สั่งจากศูนย์)

    with state_lock:
        state["last_fire"] = solution

    log_event(
        f"Fire mission [{source}] → Gun {gun_id}: AZ={az} EL={el} CHG={chg} "
        f"Range={solution['range_m']}m", "fire"
    )
    push_sse("fire_mission", solution)
    uplink.publish("fire_mission", solution)

    return solution, 200


@app.route("/api/fire_mission", methods=["POST"])
def api_fire_mission():
    body = request.get_json(force=True)
    result, code = execute_fire_mission(
        tgt_lat=body.get("tgt_lat"),
        tgt_lon=body.get("tgt_lon"),
        gun_id=body.get("gun", "1"),
        charge=body.get("charge"),     # None = auto
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
    return jsonify({"ok": True, "id": tgt_id})


@app.route("/api/target/<tgt_id>", methods=["DELETE"])
def api_del_target(tgt_id):
    with state_lock:
        removed = state["targets"].pop(tgt_id, None)
    if removed:
        push_sse("target_remove", {"id": tgt_id})
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

    open_browser_later("http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)