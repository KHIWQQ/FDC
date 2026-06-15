-- ============================================================
--  STEP D1 — schema ฐานข้อมูลศูนย์บัญชาการระดับประเทศ
--  PostgreSQL + PostGIS (เชิงพื้นที่) + TimescaleDB (time-series)
-- ============================================================
--  รันอัตโนมัติครั้งแรกโดย docker-compose (mount เข้า
--  /docker-entrypoint-initdb.d/). รันซ้ำได้ปลอดภัย (idempotent).
-- ============================================================

CREATE EXTENSION IF NOT EXISTS timescaledb;
CREATE EXTENSION IF NOT EXISTS postgis;

-- ------------------------------------------------------------
--  ทะเบียนหน่วย (1 แถวต่อ ศอย.) — สถานะปัจจุบัน + จุดที่ตั้ง
--  (ตารางสถานะ ไม่ใช่ time-series → ไม่ทำ hypertable)
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nodes (
    unit         TEXT NOT NULL,
    node         TEXT NOT NULL,
    name         TEXT,
    base_geom    geometry(Point, 4326),
    online       BOOLEAN     NOT NULL DEFAULT FALSE,
    last_seen    TIMESTAMPTZ,
    last_reason  TEXT,
    PRIMARY KEY (unit, node)
);

-- ------------------------------------------------------------
--  GPS ของ ศอย. (telemetry time-series)
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS fdc_gps (
    time   TIMESTAMPTZ      NOT NULL,
    unit   TEXT             NOT NULL,
    node   TEXT             NOT NULL,
    lat    DOUBLE PRECISION,
    lon    DOUBLE PRECISION,
    alt    DOUBLE PRECISION,
    fix    INTEGER,
    sats   INTEGER,
    bat    INTEGER,
    chg    BOOLEAN,
    geom   geometry(Point, 4326)
);
SELECT create_hypertable('fdc_gps', 'time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_fdc_gps_node ON fdc_gps (unit, node, time DESC);

-- ------------------------------------------------------------
--  สถานะหมู่ปืน (telemetry time-series)
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gun_status (
    time    TIMESTAMPTZ      NOT NULL,
    unit    TEXT             NOT NULL,
    node    TEXT             NOT NULL,
    gun_id  TEXT             NOT NULL,
    lat     DOUBLE PRECISION,
    lon     DOUBLE PRECISION,
    az      DOUBLE PRECISION,
    el      DOUBLE PRECISION,
    hdg     DOUBLE PRECISION,
    bat     INTEGER,
    rssi    INTEGER,
    snr     DOUBLE PRECISION,
    geom    geometry(Point, 4326),
    raw     JSONB
);
SELECT create_hypertable('gun_status', 'time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_gun_status_node ON gun_status (unit, node, gun_id, time DESC);

-- ------------------------------------------------------------
--  ภารกิจยิง (หลักฐานยิงที่คำนวณ/สั่ง)
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS fire_missions (
    time           TIMESTAMPTZ      NOT NULL,
    unit           TEXT             NOT NULL,
    node           TEXT             NOT NULL,
    gun_id         TEXT,
    tgt_geom       geometry(Point, 4326),
    range_m        DOUBLE PRECISION,
    az_mils        DOUBLE PRECISION,
    el_mils        DOUBLE PRECISION,
    drift_mils     DOUBLE PRECISION,
    charge         INTEGER,
    source         TEXT,
    raw            JSONB
);
SELECT create_hypertable('fire_missions', 'time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_fire_missions_node ON fire_missions (unit, node, time DESC);

-- ------------------------------------------------------------
--  เหตุการณ์ / audit log
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS events (
    time   TIMESTAMPTZ      NOT NULL,
    unit   TEXT             NOT NULL,
    node   TEXT             NOT NULL,
    level  TEXT,
    msg    TEXT,
    raw    JSONB
);
SELECT create_hypertable('events', 'time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_events_node ON events (unit, node, time DESC);

-- ------------------------------------------------------------
--  ลำดับคำสั่ง downlink ต่อ node (anti-replay ฝั่งศูนย์)
--  ศูนย์เป็นผู้ออก seq เพิ่มขึ้นเสมอ — ฝั่ง ศอย. ปฏิเสธ seq ที่ <= ล่าสุด
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS cmd_seq (
    unit  TEXT NOT NULL,
    node  TEXT NOT NULL,
    seq   BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (unit, node)
);

-- ------------------------------------------------------------
--  ผู้ใช้ + สิทธิ์ (STEP D3) — RBAC ตาม role × ขอบเขต (echelon)
--    role:        admin | commander | viewer
--                 admin     = จัดการผู้ใช้ + สั่งยิง + ดู
--                 commander = สั่งยิง + ดู (ในขอบเขต)
--                 viewer    = ดูอย่างเดียว (ในขอบเขต)  ← แยก "ดู" จาก "สั่งยิง"
--    scope_level: national | unit | node
--                 national  = เห็น/สั่งทุกหน่วยทั้งประเทศ
--                 unit      = เฉพาะหน่วย (กองพัน/กองร้อย) ของตน
--                 node      = เฉพาะ ศอย. ของตน
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    username    TEXT PRIMARY KEY,
    pw_hash     TEXT NOT NULL,
    name        TEXT,
    role        TEXT NOT NULL DEFAULT 'viewer',
    scope_level TEXT NOT NULL DEFAULT 'node',
    scope_unit  TEXT,
    scope_node  TEXT,
    disabled    BOOLEAN NOT NULL DEFAULT FALSE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login  TIMESTAMPTZ
);

-- ------------------------------------------------------------
--  Audit log (STEP D3) — บันทึกทุกคำสั่ง/การเข้าระบบ (ตรวจสอบย้อนหลังได้)
--  เก็บถาวร (ไม่ตั้ง retention) — เป็นหลักฐานสายบังคับบัญชา
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS audit_log (
    time        TIMESTAMPTZ NOT NULL DEFAULT now(),
    username    TEXT,
    action      TEXT NOT NULL,
    target_unit TEXT,
    target_node TEXT,
    result      TEXT,
    ip          TEXT,
    detail      JSONB
);
SELECT create_hypertable('audit_log', 'time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_audit_user ON audit_log (username, time DESC);

-- ------------------------------------------------------------
--  Data retention (TimescaleDB) — ตัวอย่างนโยบายเก็บข้อมูลดิบ
--  เปิดใช้ได้ตามต้องการ (telemetry ดิบเก็บ 90 วัน, เหตุการณ์ 1 ปี)
--  หมายเหตุ: เก็บสรุป/AAR ระยะยาวค่อยทำ continuous aggregate ใน D4
-- ------------------------------------------------------------
SELECT add_retention_policy('fdc_gps',    INTERVAL '90 days', if_not_exists => TRUE);
SELECT add_retention_policy('gun_status', INTERVAL '90 days', if_not_exists => TRUE);
SELECT add_retention_policy('events',     INTERVAL '365 days', if_not_exists => TRUE);
-- fire_missions: ไม่ตั้ง retention — เก็บถาวรเพื่อรายงานหลังปฏิบัติ (AAR)
