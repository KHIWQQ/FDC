# ศูนย์บัญชาการระดับประเทศ (National COP) — STEP D1

ชั้น 3 ของสถาปัตยกรรม: รวม telemetry จากทุก ศอย. ทั้งประเทศมาไว้ที่ส่วนกลาง
รองรับ deploy บน **cloud** (container ล้วน) — `docker compose up` แล้วใช้งานได้

```
ศอย. (uplink.py) ──MQTT/mTLS──▶  mosquitto  ──▶  ingest (FastAPI)  ──▶  DB (TimescaleDB+PostGIS)
                                                      │
                                                      ├─ WebSocket /ws ──▶ dashboard ผบ. (D2)
                                                      └─ REST /api/* + POST คำสั่ง downlink
```

## ส่วนประกอบ (STEP D1)

| service | image/โฟลเดอร์ | หน้าที่ |
|---------|---------------|--------|
| `db` | `timescale/timescaledb-ha:pg16` | PostgreSQL + **PostGIS** (เชิงพื้นที่) + **TimescaleDB** (time-series) |
| `mosquitto` | `eclipse-mosquitto:2` | MQTT broker, **mutual TLS เท่านั้น** + ACL ตาม CN |
| `ingest` | `ingest/` (FastAPI) | subscribe `fdc/#` → เขียน DB → push realtime + REST + ส่งคำสั่งยิง downlink |

ฐานข้อมูล (`db/schema.sql`): hypertable `fdc_gps`, `gun_status`, `fire_missions`, `events`
+ ตารางสถานะ `nodes`, ตาราง anti-replay `cmd_seq` — geometry เป็น `POINT(4326)` พร้อมใช้กับแผนที่

## เริ่มใช้งาน (dev / นำร่อง)

```bash
cd fdc/central
cp .env.example .env              # แก้รหัสผ่าน DB + ใส่ FDC_CMD_HMAC_KEY
./make-certs.sh BN1-A/FDC01       # ออกใบ mTLS: ca, broker, ingest, และของ ศอย.
docker compose up -d --build
curl http://localhost:8000/api/health
```

> **production ระดับประเทศ:** อย่าใช้ `make-certs.sh` (self-signed) — ใช้ CA/PKI ของทหาร,
> ตั้ง `BROKER_CN` ให้ตรง FQDN จริงของ broker, และเก็บ `FDC_CMD_HMAC_KEYS` ใน secret manager/HSM

## ต่อ ศอย. เข้าศูนย์

1. คัดลอกใบของหน่วยจาก `certs/BN1-A_FDC01.crt|key` + `ca.crt` ไปไว้ที่ `fdc/certs/` ของ ศอย. นั้น
2. ในเครื่อง ศอย. แก้ `fdc/config.yaml`:
   - `uplink.enabled: true`
   - `broker.host` = FQDN ของ broker (ต้องตรง CN ของ broker cert)
   - ชี้ `tls.client_cert/client_key` ไปที่ใบของหน่วย
3. ตั้ง env `FDC_CMD_HMAC_KEY` ให้ตรงกับฝั่งศูนย์ แล้วสตาร์ท ศอย. ตามปกติ

> CN ของใบ ศอย. **ต้องเป็น `<unit>/<node>`** (เช่น `BN1-A/FDC01`) — ACL ของ broker
> ใช้ CN เป็น username (`%u`) คุมว่าหน่วยนั้นเขียน `fdc/<unit>/<node>/#` และอ่าน
> `cmd/fdc/<unit>/<node>/#` ได้เท่านั้น (กันหน่วยอื่นปลอม/ดักคำสั่ง)

## Auth + RBAC (STEP D3)

ทุก endpoint (ยกเว้น `/api/health`, `/api/login`) ต้องแนบ **JWT**: `Authorization: Bearer <token>`
WebSocket ส่ง token ผ่าน query: `/ws?token=<jwt>`

**โมเดลสิทธิ์ = role × ขอบเขต (echelon):**

| role | สิทธิ์ |
|------|--------|
| `viewer` | ดูอย่างเดียว (ในขอบเขต) |
| `commander` | ดู + **สั่งยิง** (ในขอบเขต) |
| `admin` | ดู + สั่งยิง + จัดการผู้ใช้ (ทุกขอบเขต) |

| scope_level | เห็น/สั่งได้ |
|-------------|-------------|
| `national` | ทุกหน่วยทั้งประเทศ |
| `unit` | เฉพาะหน่วย (`scope_unit`) ของตน |
| `node` | เฉพาะ ศอย. (`scope_unit`/`scope_node`) ของตน |

- การกรองขอบเขตบังคับทั้ง **REST** (`/api/nodes`, `/api/fire_missions`, …) และ **WebSocket** (กรองข้อความก่อนส่งรายคน)
- **แยก "ดู" จาก "สั่งยิง"**: `viewer` เห็นทุกอย่างในขอบเขตแต่กดสั่งยิงไม่ได้ (403)
- **ทุกคำสั่งถูก audit**: ผู้ใช้/การกระทำ/เป้าหมาย/ผล/IP ลงตาราง `audit_log` (ดูที่ `GET /api/audit`, admin)
- ผู้ใช้ตั้งต้น: ตั้ง `ADMIN_USER`/`ADMIN_PASSWORD` ใน `.env` → ระบบสร้าง admin national ให้ตอนสตาร์ทครั้งแรก
- **โหมด dev**: ตั้ง `AUTH_ENABLED=false` เพื่อปิดการตรวจ (ทุก request = admin national) — ใช้ทดสอบ D1/D2 เท่านั้น

> เข้ารหัส **in-transit**: LoRa (AES) → uplink (mTLS) → API ควรอยู่หลัง reverse proxy TLS (เช่น Caddy/nginx)
> เข้ารหัส **at-rest**: วาง volume `db_data` บนดิสก์ที่เข้ารหัส (LUKS/cloud encrypted volume); รหัสผ่านผู้ใช้เก็บแบบ bcrypt

## API

| method | path | สิทธิ์ |
|--------|------|--------|
| POST | `/api/login` | สาธารณะ — คืน `{token, user}` |
| GET  | `/api/me` | ผู้ใช้ที่ล็อกอิน |
| GET  | `/api/health` | สาธารณะ |
| GET  | `/api/nodes` | view (กรองตามขอบเขต) |
| GET  | `/api/nodes/{unit}/{node}/guns` | view (ในขอบเขต) |
| GET  | `/api/targets` | view (กรองตามขอบเขต) — เป้าที่ ศอย. ปักหมุดไว้ (ยังไม่ยิง) |
| GET  | `/api/fire_missions?limit=&unit=&node=` | view (ล็อกขอบเขตอัตโนมัติ) |
| GET  | `/api/events?limit=&unit=&node=` | view (ล็อกขอบเขตอัตโนมัติ) |
| GET  | `/api/audit?limit=` | **admin** |
| GET  | `/api/reports/summary?unit=&node=&from=&to=` | view — สรุปภารกิจยิง/เหตุการณ์ |
| GET  | `/api/reports/aar/{unit}/{node}?from=&to=&format=csv` | view — รายงานหลังปฏิบัติ (JSON/CSV) |
| POST | `/api/command/{unit}/{node}/fire_mission` | **fire** (commander/admin ในขอบเขต) + audit |
| POST | `/api/command/{unit}/{node}/ping` | **fire** + audit |
| POST | `/api/command/{unit}/{node}/rescan` | **fire** + audit |
| GET/POST | `/api/users`, `/api/users/{u}/disabled` | **admin** |
| WS   | `/ws?token=<jwt>` | view (กรอง feed ตามขอบเขต) |

คำสั่ง downlink ใช้ canonical/รูปซองเดียวกับ `central/sign_command.py` และ
`FdcUplink._verify_command()` — เซ็น HMAC-SHA256 + seq (atomic จาก DB) + timestamp กัน replay

## เป้าที่ปักหมุด (target sharing)

ศอย. ปักหมุดเป้า (`POST /api/target` ฝั่ง ศอย.) → uplink ขึ้นศูนย์ทันที **ตั้งแต่ปักหมุด
(ก่อนยิง)** บน topic `fdc/<unit>/<node>/target/<id>` (retained = current state; payload
`active:false` = ลบ). ที่ศูนย์ `mqtt_ingest` → ตาราง `targets` → push WebSocket →
dashboard โชว์หมุด ⊕ สีเหลืองอำพัน (แยกจาก 🎯 ภารกิจยิงที่เกิดขึ้นจริง). โหลดสถานะเดิม
ผ่าน `GET /api/targets` + รวมใน WebSocket snapshot (กรองตามขอบเขต RBAC เหมือน telemetry อื่น)

## รายงาน / AAR (STEP D4)

- **continuous aggregates** (`db/20-aggregates.sql`): `fire_missions_daily`, `events_daily`,
  `gun_battery_hourly` — สรุปล่วงหน้า รายงานเร็วแม้ telemetry ดิบเยอะ (refresh policy อัตโนมัติ)
- **สรุปภารกิจ**: `GET /api/reports/summary` — จำนวนภารกิจ, ระยะ/ดินเฉลี่ย, แยกตามกระบอก, นับ error
- **AAR export**: `GET /api/reports/aar/{unit}/{node}?format=csv` — ไทม์ไลน์ภารกิจยิง+เหตุการณ์
  ดาวน์โหลดเป็น CSV (การ export ถูกบันทึก audit) — ปุ่ม ⬇ CSV อยู่ในแผง "รายละเอียดหน่วย" บน dashboard
- ภารกิจยิง (`fire_missions`) เก็บถาวร (ไม่มี retention) เพื่อใช้รายงานย้อนหลัง

## ทดสอบทั้งระบบโดยไม่มี hardware (mock)

ยังไม่มี gun unit/dongle ก็รัน D1–D4 ครบได้ ด้วยตัวจำลอง ศอย. ส่ง telemetry ขึ้น broker:

```bash
cd fdc/central
cp .env.example .env                         # ตั้ง DB pw, JWT_SECRET, ADMIN_PASSWORD
./make-certs.sh BN1-A/FDC01                  # ออกใบ mTLS (รวมใบของ ศอย. นี้)
docker compose up -d --build

pip install paho-mqtt
python3 mock_publisher.py --unit BN1-A --node FDC01 \
    --host localhost --port 8883 \
    --ca certs/ca.crt --cert certs/BN1-A_FDC01.crt --key certs/BN1-A_FDC01.key
```

เปิด `http://localhost:8000/` → ล็อกอินด้วย admin → จะเห็นหมุด ศอย./หมู่ปืนขยับ,
ภารกิจยิงโผล่เป็นระยะ, replay/AAR มีข้อมูลให้ดู
(broker.crt ออกด้วย CN=localhost จึงต้องใช้ `--host localhost`; เครื่องอื่นใช้ `--insecure` หรือออกใบใหม่ตาม FQDN)

## ก้อน D — เสร็จครบ (D1–D4) ✅
ขั้นต่อไปนอกก้อน D (ตาม ROADMAP): **B3** addressing ลำดับชั้นฝั่งแม่ข่าย/เว็บ, **B4** ตารางยิงจริง (รอข้อมูลชนิดปืน)
