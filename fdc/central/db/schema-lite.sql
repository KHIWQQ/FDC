-- ============================================================
--  schema-lite — สำหรับรันบน Raspberry Pi / arm64
--  PostgreSQL + PostGIS เท่านั้น (ไม่มี TimescaleDB)
--  ตารางเหมือน schema.sql ทุกอย่าง แต่เป็น "ตารางธรรมดา"
--  (ไม่ทำ hypertable/retention/continuous aggregate)
--  D1–D4 ทำงานครบ — แค่ไม่มีการปรับแต่ง time-series ของ TimescaleDB
-- ============================================================

CREATE EXTENSION IF NOT EXISTS postgis;

-- ทะเบียนหน่วย (ศอย.)
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

-- GPS ของ ศอย.
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
CREATE INDEX IF NOT EXISTS idx_fdc_gps_node ON fdc_gps (unit, node, time DESC);

-- สถานะหมู่ปืน
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
CREATE INDEX IF NOT EXISTS idx_gun_status_node ON gun_status (unit, node, gun_id, time DESC);

-- ภารกิจยิง
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
CREATE INDEX IF NOT EXISTS idx_fire_missions_node ON fire_missions (unit, node, time DESC);

-- เหตุการณ์ / audit telemetry
CREATE TABLE IF NOT EXISTS events (
    time   TIMESTAMPTZ      NOT NULL,
    unit   TEXT             NOT NULL,
    node   TEXT             NOT NULL,
    level  TEXT,
    msg    TEXT,
    raw    JSONB
);
CREATE INDEX IF NOT EXISTS idx_events_node ON events (unit, node, time DESC);

-- ลำดับคำสั่ง downlink (anti-replay)
CREATE TABLE IF NOT EXISTS cmd_seq (
    unit  TEXT NOT NULL,
    node  TEXT NOT NULL,
    seq   BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (unit, node)
);

-- ผู้ใช้ + สิทธิ์ (STEP D3)
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

-- Audit log (STEP D3) — ตารางธรรมดา
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
CREATE INDEX IF NOT EXISTS idx_audit_user ON audit_log (username, time DESC);
