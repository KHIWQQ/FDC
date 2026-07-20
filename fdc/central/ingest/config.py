"""
config.py — โหลดค่าคอนฟิกของ ingest service จาก environment (12-factor / cloud)
ทุกค่าอ่านจาก env เพื่อให้ deploy บน cloud / container ได้โดยไม่แก้โค้ด
"""
import os
import json

try:
    from dotenv import load_dotenv
    load_dotenv()          # โหลด .env ถ้ามี (ตอน dev) — บน cloud ใช้ env จริง
except ImportError:
    pass


def _bool(name: str, default: bool) -> bool:
    v = os.environ.get(name)
    if v is None:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


# ---- ฐานข้อมูล (PostgreSQL/PostGIS/TimescaleDB) ----
DB_DSN = os.environ.get(
    "DB_DSN",
    "postgresql://fdc:fdc@db:5432/fdc",
)

# ---- MQTT broker ----
MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "8883"))
MQTT_KEEPALIVE = int(os.environ.get("MQTT_KEEPALIVE", "30"))
MQTT_CLIENT_ID = os.environ.get("MQTT_CLIENT_ID", "central-ingest")
# subscribe ทุก ศอย. ใต้ namespace fdc/
MQTT_TOPIC = os.environ.get("MQTT_TOPIC", "fdc/#")

# ---- mTLS (ingest เป็น client ที่เชื่อถือได้ของ broker ภายในศูนย์) ----
MQTT_TLS = _bool("MQTT_TLS", True)
MQTT_CA_CERT = os.environ.get("MQTT_CA_CERT", "/certs/ca.crt")
MQTT_CLIENT_CERT = os.environ.get("MQTT_CLIENT_CERT", "/certs/ingest.crt")
MQTT_CLIENT_KEY = os.environ.get("MQTT_CLIENT_KEY", "/certs/ingest.key")
MQTT_TLS_INSECURE = _bool("MQTT_TLS_INSECURE", False)   # dev เท่านั้น: ข้ามตรวจชื่อ host

# ---- คีย์เซ็นคำสั่ง downlink (HMAC-SHA256) ----
# รองรับหลายหน่วย: FDC_CMD_HMAC_KEYS = JSON {"BN1-A":"<hex>","BN1-B":"<hex>"}
# fallback คีย์เดี่ยว: FDC_CMD_HMAC_KEY = "<hex>"
def _load_cmd_keys() -> dict:
    keys = {}
    raw = os.environ.get("FDC_CMD_HMAC_KEYS", "")
    if raw:
        try:
            for unit, hexk in json.loads(raw).items():
                keys[str(unit)] = bytes.fromhex(hexk)
        except Exception as e:
            raise RuntimeError(f"FDC_CMD_HMAC_KEYS invalid: {e}")
    single = os.environ.get("FDC_CMD_HMAC_KEY", "")
    if single:
        keys["*"] = bytes.fromhex(single)      # คีย์ default ใช้เมื่อไม่มีคีย์เฉพาะหน่วย
    return keys


CMD_HMAC_KEYS = _load_cmd_keys()


def cmd_key_for(unit: str) -> bytes | None:
    return CMD_HMAC_KEYS.get(unit) or CMD_HMAC_KEYS.get("*")


# ---- เซิร์ฟเวอร์ API ----
API_HOST = os.environ.get("API_HOST", "0.0.0.0")
API_PORT = int(os.environ.get("API_PORT", "8000"))

# ---- Auth / RBAC (STEP D3) ----
# AUTH_ENABLED=false → เปิด API ให้ทุกคน (โหมด dev/ทดสอบ D1/D2 เท่านั้น)
AUTH_ENABLED = _bool("AUTH_ENABLED", True)
JWT_SECRET = os.environ.get("JWT_SECRET", "")
JWT_TTL_MIN = int(os.environ.get("JWT_TTL_MIN", "720"))     # อายุ token (นาที) ~12 ชม.
JWT_ALG = "HS256"
# ผู้ดูแลตั้งต้น — สร้างให้อัตโนมัติตอนสตาร์ทถ้ายังไม่มีผู้ใช้เลย
ADMIN_USER = os.environ.get("ADMIN_USER", "admin")
ADMIN_PASSWORD = os.environ.get("ADMIN_PASSWORD", "")

if AUTH_ENABLED and not JWT_SECRET:
    raise RuntimeError(
        "AUTH_ENABLED=true แต่ไม่ได้ตั้ง JWT_SECRET — "
        "ตั้งค่า JWT_SECRET (สุ่มยาว) ก่อน หรือ AUTH_ENABLED=false สำหรับ dev")


# ---- โหมดสาธิต (ปุ่ม DEMO) ----
# ป้อน mock telemetry ฝั่งศูนย์เพื่อโชว์ dashboard โดยไม่ต้องมี ศอย./ฮาร์ดแวร์จริง
# ปิดในระบบจริง: DEMO_ENABLED=false
DEMO_ENABLED = _bool("DEMO_ENABLED", True)


# ---- ไฟล์ dashboard (STEP D2) ----
def _web_dir() -> str | None:
    """หาโฟลเดอร์ web/ ให้ทำงานได้ทั้งใน container (/app/web) และ dev (../web)."""
    env = os.environ.get("WEB_DIR")
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [env] if env else []
    candidates += [
        os.path.join(here, "web"),                       # ingest/web (ถ้า copy เข้าไป)
        os.path.join(here, "..", "web"),                 # central/web (dev)
        "/app/web",                                      # mount ใน container
    ]
    for c in candidates:
        if c and os.path.isdir(c):
            return os.path.abspath(c)
    return None


WEB_DIR = _web_dir()
