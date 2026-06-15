"""
uplink.py — MQTT uplink ศอย. → ศูนย์บัญชาการ (STEP C1)

ทำไม MQTT (ไม่ใช่ HTTPS) สำหรับชั้นนี้:
  - downlink push: ศูนย์สั่งภารกิจยิงลงมาได้ทันที (HTTPS ต้อง poll)
  - ลิงก์ไม่เสถียร (4G/SATCOM): persistent session + auto-reconnect + QoS
  - LWT: ตรวจ node ตายได้เองทันที
  - store-and-forward: เน็ตหลุดเก็บคิว ส่งซ้ำเมื่อกลับ — ข้อมูลไม่หาย

ความปลอดภัย (military grade):
  - mutual TLS (mTLS): ยืนยันตัวตนสองทาง broker ↔ node, TLS 1.2+ กัน downgrade
  - HMAC-SHA256 + sequence + timestamp บน "คำสั่งยิง downlink":
    กันปลอม/กันบันทึกแล้วส่งซ้ำ (anti-replay) แม้ broker ถูกเจาะ — defense in depth
  - last-seq เก็บลง SQLite → anti-replay รอดแม้ restart

Edge autonomy: ถ้า paho ไม่ติดตั้ง หรือ uplink.enabled=false → ทุกเมธอดเป็น no-op
ศอย. ยังรับพิกัด/คำนวณ/สั่งยิงได้ครบโดยไม่พึ่งเน็ต
"""

import os
import ssl
import json
import time
import hmac
import hashlib
import sqlite3
import logging
import threading

logger = logging.getLogger("uplink")

try:
    import paho.mqtt.client as mqtt
    _HAVE_PAHO = True
except ImportError:
    _HAVE_PAHO = False

_TLS_VERSIONS = {
    "TLSv1.2": ssl.TLSVersion.TLSv1_2,
    "TLSv1.3": ssl.TLSVersion.TLSv1_3,
}


class FdcUplink:
    """MQTT uplink client with mTLS, anti-replay downlink, and offline store-and-forward."""

    def __init__(self, config: dict):
        up = (config or {}).get("uplink", {}) or {}
        ident = (config or {}).get("identity", {}) or {}

        self.enabled = bool(up.get("enabled", False))
        self.unit = str(ident.get("unit", "UNIT"))
        self.node = str(ident.get("node", "NODE"))
        self.base = f"fdc/{self.unit}/{self.node}"
        self.cmd_topic = f"cmd/{self.base}/#"

        self._cfg = up
        self._broker = up.get("broker", {}) or {}
        self._tls = up.get("tls", {}) or {}
        self._cmd_auth = up.get("command_auth", {}) or {}
        self._qos = int(up.get("publish_qos", 1))

        self._client = None
        self._connected = False
        self._stop = threading.Event()
        self._lock = threading.Lock()

        # outbox worker
        self._db_path = (up.get("queue", {}) or {}).get("db_path", "uplink_queue.db")
        self._max_rows = int((up.get("queue", {}) or {}).get("max_rows", 50000))
        self._db = None
        self._pending = {}          # mqtt mid → outbox rowid (awaiting PUBACK)
        self._flush_wake = threading.Event()
        self._worker = None

        # HMAC command key (hex) — โหลดจาก env เท่านั้น ไม่อ่านจากไฟล์ config
        key_env = self._cmd_auth.get("hmac_key_env", "FDC_CMD_HMAC_KEY")
        key_hex = os.environ.get(key_env, "")
        self._hmac_key = bytes.fromhex(key_hex) if key_hex else None
        self._max_skew = int(self._cmd_auth.get("max_skew_sec", 30))

        # callbacks set by host (main.py)
        self.command_callback = None    # fn(cmd_type:str, data:dict)
        self.status_callback = None     # fn(connected:bool)

    # ------------------------------------------------------------------
    #  Lifecycle
    # ------------------------------------------------------------------

    def start(self):
        if not self.enabled:
            logger.info("Uplink disabled (config) — running standalone / edge-autonomous")
            return
        if not _HAVE_PAHO:
            logger.warning("paho-mqtt not installed — uplink stays OFFLINE "
                           "(pip install paho-mqtt). ศอย. ยังทำงานครบในเครื่อง")
            return

        self._init_db()
        self._worker = threading.Thread(target=self._outbox_loop, daemon=True)
        self._worker.start()

        self._client = mqtt.Client(
            client_id=f"{self.unit}-{self.node}",
            clean_session=False,            # persistent session: ไม่พลาด downlink ตอนหลุดสั้นๆ
            protocol=mqtt.MQTTv311,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._client.on_publish = self._on_publish

        # Last Will & Testament — broker ดันให้ศูนย์เองถ้า node นี้ขาดการเชื่อมต่อ
        will = json.dumps({"online": False, "unit": self.unit, "node": self.node,
                           "reason": "lost"})
        self._client.will_set(f"{self.base}/status", will, qos=1, retain=True)

        self._setup_tls()

        host = self._broker.get("host", "localhost")
        port = int(self._broker.get("port", 8883))
        keepalive = int(self._broker.get("keepalive", 30))
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)
        try:
            self._client.connect_async(host, port, keepalive)
            self._client.loop_start()
            logger.info(f"Uplink connecting → {host}:{port} as {self.unit}/{self.node}")
        except Exception as e:
            logger.error(f"Uplink connect failed: {e} — will keep retrying in background")

    def stop(self):
        self._stop.set()
        self._flush_wake.set()
        if self._client:
            try:
                # graceful offline status (retained) ก่อนตัด
                self._client.publish(f"{self.base}/status",
                                     json.dumps({"online": False, "unit": self.unit,
                                                 "node": self.node, "reason": "shutdown"}),
                                     qos=1, retain=True)
                self._client.loop_stop()
                self._client.disconnect()
            except Exception:
                pass

    @property
    def connected(self) -> bool:
        return self._connected

    def status(self) -> dict:
        return {
            "enabled": self.enabled,
            "available": _HAVE_PAHO,
            "connected": self._connected,
            "unit": self.unit,
            "node": self.node,
            "broker": self._broker.get("host") if self.enabled else None,
            "queued": self._queue_depth(),
        }

    # ------------------------------------------------------------------
    #  TLS (mutual)
    # ------------------------------------------------------------------

    def _setup_tls(self):
        ca = self._resolve(self._tls.get("ca_cert"))
        crt = self._resolve(self._tls.get("client_cert"))
        key = self._resolve(self._tls.get("client_key"))
        if not (ca and crt and key):
            raise RuntimeError("mTLS requires ca_cert, client_cert and client_key — "
                               "refusing to connect without them (no plaintext uplink)")
        for label, path in (("ca_cert", ca), ("client_cert", crt), ("client_key", key)):
            if not os.path.isfile(path):
                raise FileNotFoundError(f"TLS {label} not found: {path}")

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = _TLS_VERSIONS.get(self._tls.get("min_version", "TLSv1.2"),
                                                ssl.TLSVersion.TLSv1_2)
        ctx.verify_mode = ssl.CERT_REQUIRED        # ตรวจ broker เสมอ
        ctx.check_hostname = True                  # กัน MITM ด้วยชื่อในใบรับรอง
        ctx.load_verify_locations(cafile=ca)
        ctx.load_cert_chain(certfile=crt, keyfile=key)   # client cert → mutual auth
        self._client.tls_set_context(ctx)

    def _resolve(self, path):
        if not path:
            return None
        if os.path.isabs(path):
            return path
        return os.path.join(os.path.dirname(os.path.abspath(__file__)), path)

    # ------------------------------------------------------------------
    #  Publish (store-and-forward)
    # ------------------------------------------------------------------

    def publish(self, subtopic: str, payload: dict, retain: bool = False):
        """Enqueue a telemetry message. Persisted first → survives crash/offline."""
        if not self.enabled or not _HAVE_PAHO:
            return
        topic = f"{self.base}/{subtopic}"
        body = json.dumps(payload, separators=(",", ":"))
        self._enqueue(topic, body, self._qos, 1 if retain else 0)
        self._flush_wake.set()

    # ------------------------------------------------------------------
    #  Outbox: SQLite-backed store-and-forward
    # ------------------------------------------------------------------

    def _init_db(self):
        path = self._resolve(self._db_path)
        # check_same_thread=False: เข้าถึงจากหลาย thread ผ่าน self._lock
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.execute("""CREATE TABLE IF NOT EXISTS outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            topic TEXT NOT NULL, payload TEXT NOT NULL,
            qos INTEGER NOT NULL, retain INTEGER NOT NULL, ts REAL NOT NULL)""")
        self._db.execute("""CREATE TABLE IF NOT EXISTS kv (
            k TEXT PRIMARY KEY, v TEXT NOT NULL)""")
        self._db.commit()

    def _enqueue(self, topic, payload, qos, retain):
        with self._lock:
            self._db.execute(
                "INSERT INTO outbox(topic,payload,qos,retain,ts) VALUES(?,?,?,?,?)",
                (topic, payload, qos, retain, self._now()))
            # cap: ทิ้งของเก่าสุดถ้าเกินเพดาน (กันดิสก์เต็มในสนาม)
            cur = self._db.execute("SELECT COUNT(*) FROM outbox").fetchone()[0]
            if cur > self._max_rows:
                self._db.execute(
                    "DELETE FROM outbox WHERE id IN "
                    "(SELECT id FROM outbox ORDER BY id LIMIT ?)",
                    (cur - self._max_rows,))
            self._db.commit()

    def _queue_depth(self) -> int:
        if not self._db:
            return 0
        try:
            with self._lock:
                return self._db.execute("SELECT COUNT(*) FROM outbox").fetchone()[0]
        except Exception:
            return 0

    def _outbox_loop(self):
        """Drain the outbox whenever connected; block when offline."""
        while not self._stop.is_set():
            if not self._connected:
                self._flush_wake.wait(2.0)
                self._flush_wake.clear()
                continue
            rows = []
            with self._lock:
                rows = self._db.execute(
                    "SELECT id,topic,payload,qos,retain FROM outbox "
                    "ORDER BY id LIMIT 200").fetchall()
            if not rows:
                self._flush_wake.wait(1.0)
                self._flush_wake.clear()
                continue
            for rid, topic, payload, qos, retain in rows:
                if not self._connected:
                    break
                try:
                    info = self._client.publish(topic, payload, qos=qos, retain=bool(retain))
                except Exception as e:
                    logger.debug(f"publish error: {e}")
                    break
                if info.rc != mqtt.MQTT_ERR_SUCCESS:
                    break
                if qos == 0:
                    # ไม่มี PUBACK — ถือว่าส่งแล้ว ลบทันที
                    self._delete_row(rid)
                else:
                    # QoS1: ลบเมื่อได้ PUBACK (on_publish) เท่านั้น
                    with self._lock:
                        self._pending[info.mid] = rid
            time.sleep(0.05)

    def _delete_row(self, rid):
        with self._lock:
            self._db.execute("DELETE FROM outbox WHERE id=?", (rid,))
            self._db.commit()

    # ------------------------------------------------------------------
    #  MQTT callbacks
    # ------------------------------------------------------------------

    def _on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            logger.warning(f"Uplink connect rc={rc}")
            return
        self._connected = True
        logger.info("Uplink CONNECTED to central")
        client.subscribe(self.cmd_topic, qos=1)
        client.publish(f"{self.base}/status",
                       json.dumps({"online": True, "unit": self.unit, "node": self.node,
                                   "ts": self._now()}),
                       qos=1, retain=True)
        self._flush_wake.set()
        if self.status_callback:
            try: self.status_callback(True)
            except Exception: pass

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        logger.warning(f"Uplink DISCONNECTED (rc={rc}) — queuing locally, will retry")
        if self.status_callback:
            try: self.status_callback(False)
            except Exception: pass

    def _on_publish(self, client, userdata, mid):
        with self._lock:
            rid = self._pending.pop(mid, None)
        if rid is not None:
            self._delete_row(rid)

    def _on_message(self, client, userdata, msg):
        """Downlink command — verify HMAC + anti-replay BEFORE acting."""
        try:
            env = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            logger.warning("Downlink: bad JSON, dropped")
            return

        ok, reason = self._verify_command(env)
        if not ok:
            logger.warning(f"Downlink REJECTED ({reason}) topic={msg.topic}")
            return

        cmd_type = env.get("type")
        data = env.get("data", {})
        logger.info(f"Downlink ACCEPTED: {cmd_type} seq={env.get('seq')}")
        if self.command_callback:
            try:
                self.command_callback(cmd_type, data)
            except Exception as e:
                logger.error(f"command_callback error: {e}")

    # ------------------------------------------------------------------
    #  Command authentication (HMAC-SHA256 + seq + timestamp)
    # ------------------------------------------------------------------

    def _verify_command(self, env: dict):
        if self._hmac_key is None:
            return False, "no HMAC key configured (set FDC_CMD_HMAC_KEY)"
        sig = env.get("sig")
        if not sig:
            return False, "missing signature"

        ts = env.get("ts")
        if not isinstance(ts, (int, float)):
            return False, "missing/invalid ts"
        if abs(self._now() - ts) > self._max_skew:
            return False, "stale timestamp (replay?)"

        seq = env.get("seq")
        if not isinstance(seq, int):
            return False, "missing/invalid seq"
        last = self._get_last_seq()
        if seq <= last:
            return False, f"replayed seq {seq} <= {last}"

        # canonical string — ต้องตรงกับฝั่งศูนย์ที่เซ็น (เรียงคีย์, ไม่มี space)
        canon = "{}|{}|{}|{}".format(
            env.get("type", ""), seq, ts,
            json.dumps(env.get("data", {}), sort_keys=True, separators=(",", ":")))
        expect = hmac.new(self._hmac_key, canon.encode("utf-8"),
                          hashlib.sha256).hexdigest()
        if not hmac.compare_digest(expect, str(sig)):
            return False, "bad HMAC"

        self._set_last_seq(seq)      # commit seq เฉพาะเมื่อผ่านครบ
        return True, "ok"

    def _get_last_seq(self) -> int:
        if not self._db:
            return 0
        with self._lock:
            row = self._db.execute("SELECT v FROM kv WHERE k='last_cmd_seq'").fetchone()
        return int(row[0]) if row else 0

    def _set_last_seq(self, seq: int):
        with self._lock:
            self._db.execute("INSERT INTO kv(k,v) VALUES('last_cmd_seq',?) "
                             "ON CONFLICT(k) DO UPDATE SET v=excluded.v", (str(seq),))
            self._db.commit()

    # ------------------------------------------------------------------

    @staticmethod
    def _now() -> float:
        return time.time()
