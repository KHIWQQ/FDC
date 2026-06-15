"""
db.py — ชั้นเข้าถึงฐานข้อมูล (asyncpg pool) สำหรับ ingest + API
  - เขียน telemetry ที่เข้ามาจาก MQTT ลง hypertable
  - คิวรีสถานะปัจจุบัน/ประวัติ ให้ REST + dashboard
geom: สร้างจาก lon/lat เมื่อมีพิกัดจริงเท่านั้น (0,0 = ยังไม่จับ fix → เก็บ NULL)
"""
import json
import logging
import asyncpg

import config

logger = logging.getLogger("db")

_pool: asyncpg.Pool | None = None


async def connect():
    global _pool
    _pool = await asyncpg.create_pool(config.DB_DSN, min_size=2, max_size=10)
    logger.info("DB pool connected")


async def close():
    if _pool:
        await _pool.close()


def _has_fix(lat, lon) -> bool:
    return bool(lat) or bool(lon)      # 0,0 → ยังไม่มีพิกัด


# ------------------------------------------------------------
#  เขียน telemetry (เรียกจาก mqtt_ingest)
# ------------------------------------------------------------

async def upsert_node_status(unit, node, online, ts, reason=None, name=None):
    """อัปเดตสถานะ online/last_seen ของ ศอย. (จาก .../status + LWT)"""
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO nodes (unit, node, name, online, last_seen, last_reason)
               VALUES ($1,$2,$3,$4, to_timestamp($5), $6)
               ON CONFLICT (unit, node) DO UPDATE SET
                 online      = EXCLUDED.online,
                 last_seen   = COALESCE(EXCLUDED.last_seen, nodes.last_seen),
                 last_reason = EXCLUDED.last_reason,
                 name        = COALESCE(EXCLUDED.name, nodes.name)""",
            unit, node, name, online, ts, reason,
        )


async def insert_fdc_gps(unit, node, ts, d: dict):
    lat, lon = d.get("lat"), d.get("lon")
    geom = f"SRID=4326;POINT({lon} {lat})" if _has_fix(lat, lon) else None
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO fdc_gps (time, unit, node, lat, lon, alt, fix, sats, bat, chg, geom)
               VALUES (to_timestamp($1),$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)""",
            ts, unit, node, lat, lon, d.get("alt"), d.get("fix"),
            d.get("sats"), d.get("bat"), d.get("chg"), geom,
        )
        # อัปเดต last_seen ของ node ไปด้วย (telemetry = ยังมีชีวิต)
        await c.execute(
            """INSERT INTO nodes (unit, node, online, last_seen)
               VALUES ($1,$2,TRUE,to_timestamp($3))
               ON CONFLICT (unit, node) DO UPDATE SET
                 online=TRUE, last_seen=to_timestamp($3)""",
            unit, node, ts,
        )


async def insert_gun_status(unit, node, gun_id, ts, d: dict):
    lat, lon = d.get("lat"), d.get("lon")
    geom = f"SRID=4326;POINT({lon} {lat})" if _has_fix(lat, lon) else None
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO gun_status
               (time, unit, node, gun_id, lat, lon, az, el, hdg, bat, rssi, snr, geom, raw)
               VALUES (to_timestamp($1),$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14)""",
            ts, unit, node, gun_id, lat, lon, d.get("az"), d.get("el"),
            d.get("hdg"), d.get("bat"), d.get("rssi"), d.get("snr"),
            geom, json.dumps(d),
        )


async def insert_fire_mission(unit, node, ts, d: dict):
    lat, lon = d.get("tgt_lat"), d.get("tgt_lon")
    geom = f"SRID=4326;POINT({lon} {lat})" if _has_fix(lat, lon) else None
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO fire_missions
               (time, unit, node, gun_id, tgt_geom, range_m, az_mils, el_mils,
                drift_mils, charge, source, raw)
               VALUES (to_timestamp($1),$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)""",
            ts, unit, node, str(d.get("gun")) if d.get("gun") is not None else None,
            geom, d.get("range_m"), d.get("az_mils"), d.get("el_mils"),
            d.get("drift_mils"), d.get("charge"), d.get("source"), json.dumps(d),
        )


async def insert_event(unit, node, ts, d: dict):
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO events (time, unit, node, level, msg, raw)
               VALUES (to_timestamp($1),$2,$3,$4,$5,$6)""",
            ts, unit, node, d.get("level"), d.get("msg"), json.dumps(d),
        )


# ------------------------------------------------------------
#  ลำดับคำสั่ง downlink (anti-replay) — ออก seq ใหม่แบบ atomic
# ------------------------------------------------------------

async def next_cmd_seq(unit, node) -> int:
    async with _pool.acquire() as c:
        row = await c.fetchrow(
            """INSERT INTO cmd_seq (unit, node, seq) VALUES ($1,$2,1)
               ON CONFLICT (unit, node) DO UPDATE SET seq = cmd_seq.seq + 1
               RETURNING seq""",
            unit, node,
        )
        return int(row["seq"])


# ------------------------------------------------------------
#  คิวรีสำหรับ REST / dashboard
# ------------------------------------------------------------

async def list_nodes() -> list[dict]:
    """ทุก ศอย. + พิกัด FDC ล่าสุด (สำหรับหมุดบนแผนที่ประเทศ)"""
    async with _pool.acquire() as c:
        rows = await c.fetch(
            """SELECT n.unit, n.node, n.name, n.online, n.last_seen, n.last_reason,
                      ST_Y(n.base_geom) AS base_lat, ST_X(n.base_geom) AS base_lon,
                      g.lat, g.lon, g.alt, g.fix, g.sats, g.bat, g.chg,
                      g.time AS gps_time
               FROM nodes n
               LEFT JOIN LATERAL (
                   SELECT lat, lon, alt, fix, sats, bat, chg, time
                   FROM fdc_gps f
                   WHERE f.unit = n.unit AND f.node = n.node
                   ORDER BY time DESC LIMIT 1
               ) g ON TRUE
               ORDER BY n.unit, n.node"""
        )
        return [dict(r) for r in rows]


async def latest_guns(unit, node) -> list[dict]:
    """สถานะหมู่ปืนล่าสุดของแต่ละกระบอกใน ศอย. หนึ่ง"""
    async with _pool.acquire() as c:
        rows = await c.fetch(
            """SELECT DISTINCT ON (gun_id)
                      gun_id, time, lat, lon, az, el, hdg, bat, rssi, snr
               FROM gun_status
               WHERE unit=$1 AND node=$2
               ORDER BY gun_id, time DESC""",
            unit, node,
        )
        return [dict(r) for r in rows]


async def recent_fire_missions(limit=100, unit=None, node=None) -> list[dict]:
    q = ("""SELECT time, unit, node, gun_id, ST_Y(tgt_geom) AS tgt_lat,
                   ST_X(tgt_geom) AS tgt_lon, range_m, az_mils, el_mils,
                   drift_mils, charge, source
            FROM fire_missions""")
    args, where = [], []
    if unit:
        args.append(unit); where.append(f"unit=${len(args)}")
    if node:
        args.append(node); where.append(f"node=${len(args)}")
    if where:
        q += " WHERE " + " AND ".join(where)
    args.append(limit)
    q += f" ORDER BY time DESC LIMIT ${len(args)}"
    async with _pool.acquire() as c:
        rows = await c.fetch(q, *args)
        return [dict(r) for r in rows]


# ------------------------------------------------------------
#  ผู้ใช้ + audit (STEP D3)
# ------------------------------------------------------------

async def count_users() -> int:
    async with _pool.acquire() as c:
        return int(await c.fetchval("SELECT COUNT(*) FROM users"))


async def get_user(username: str) -> dict | None:
    async with _pool.acquire() as c:
        row = await c.fetchrow(
            """SELECT username, pw_hash, name, role, scope_level,
                      scope_unit, scope_node, disabled, last_login
               FROM users WHERE username=$1""", username)
        return dict(row) if row else None


async def create_user(username, pw_hash, name, role, scope_level,
                      scope_unit=None, scope_node=None) -> None:
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO users
               (username, pw_hash, name, role, scope_level, scope_unit, scope_node)
               VALUES ($1,$2,$3,$4,$5,$6,$7)""",
            username, pw_hash, name, role, scope_level, scope_unit, scope_node)


async def set_last_login(username: str) -> None:
    async with _pool.acquire() as c:
        await c.execute("UPDATE users SET last_login=now() WHERE username=$1", username)


async def set_user_disabled(username: str, disabled: bool) -> None:
    async with _pool.acquire() as c:
        await c.execute("UPDATE users SET disabled=$2 WHERE username=$1",
                        username, disabled)


async def list_users() -> list[dict]:
    async with _pool.acquire() as c:
        rows = await c.fetch(
            """SELECT username, name, role, scope_level, scope_unit, scope_node,
                      disabled, created_at, last_login
               FROM users ORDER BY username""")
        return [dict(r) for r in rows]


async def insert_audit(username, action, result, target_unit=None,
                       target_node=None, ip=None, detail=None) -> None:
    async with _pool.acquire() as c:
        await c.execute(
            """INSERT INTO audit_log
               (username, action, target_unit, target_node, result, ip, detail)
               VALUES ($1,$2,$3,$4,$5,$6,$7)""",
            username, action, target_unit, target_node, result, ip,
            json.dumps(detail) if detail is not None else None)


async def recent_audit(limit=200) -> list[dict]:
    async with _pool.acquire() as c:
        rows = await c.fetch(
            """SELECT time, username, action, target_unit, target_node, result, ip, detail
               FROM audit_log ORDER BY time DESC LIMIT $1""", min(limit, 1000))
        return [dict(r) for r in rows]


# ------------------------------------------------------------
#  รายงาน / AAR (STEP D4)
# ------------------------------------------------------------

def _scope_range(unit, node, frm, to, start_at=0):
    """สร้าง args + เงื่อนไข WHERE สำหรับ unit/node + ช่วงเวลา (frm/to เป็น ISO/timestamptz)"""
    args, where = [], []
    if unit:
        args.append(unit); where.append(f"unit=${len(args)+start_at}")
    if node:
        args.append(node); where.append(f"node=${len(args)+start_at}")
    if frm:
        args.append(frm); where.append(f"time >= ${len(args)+start_at}::timestamptz")
    if to:
        args.append(to); where.append(f"time <= ${len(args)+start_at}::timestamptz")
    clause = (" WHERE " + " AND ".join(where)) if where else ""
    return args, clause


async def fire_mission_stats(unit=None, node=None, frm=None, to=None) -> dict:
    args, w = _scope_range(unit, node, frm, to)
    async with _pool.acquire() as c:
        total = await c.fetchrow(
            f"""SELECT count(*) AS missions, avg(range_m) AS avg_range_m,
                       avg(charge) AS avg_charge, min(time) AS first, max(time) AS last
                FROM fire_missions{w}""", *args)
        by_gun = await c.fetch(
            f"""SELECT gun_id, count(*) AS missions, avg(range_m) AS avg_range_m
                FROM fire_missions{w} GROUP BY gun_id ORDER BY gun_id""", *args)
        by_charge = await c.fetch(
            f"""SELECT charge, count(*) AS missions
                FROM fire_missions{w} GROUP BY charge ORDER BY charge""", *args)
    return {"total": dict(total), "by_gun": [dict(r) for r in by_gun],
            "by_charge": [dict(r) for r in by_charge]}


async def event_stats(unit=None, node=None, frm=None, to=None) -> list[dict]:
    args, w = _scope_range(unit, node, frm, to)
    async with _pool.acquire() as c:
        rows = await c.fetch(
            f"SELECT level, count(*) AS n FROM events{w} GROUP BY level ORDER BY n DESC",
            *args)
    return [dict(r) for r in rows]


async def aar_timeline(unit, node, frm=None, to=None, limit=2000) -> list[dict]:
    """รวมภารกิจยิง + เหตุการณ์ เรียงตามเวลา = ไทม์ไลน์หลังปฏิบัติ (AAR)"""
    fa, fw = _scope_range(unit, node, frm, to)
    ea, ew = _scope_range(unit, node, frm, to)
    async with _pool.acquire() as c:
        fms = await c.fetch(
            f"""SELECT time, gun_id, ST_Y(tgt_geom) AS tgt_lat, ST_X(tgt_geom) AS tgt_lon,
                       range_m, az_mils, el_mils, charge, source
                FROM fire_missions{fw} ORDER BY time DESC LIMIT {int(limit)}""", *fa)
        evs = await c.fetch(
            f"""SELECT time, level, msg FROM events{ew}
                ORDER BY time DESC LIMIT {int(limit)}""", *ea)
    items = []
    for r in fms:
        d = dict(r); d["kind"] = "fire_mission"; items.append(d)
    for r in evs:
        d = dict(r); d["kind"] = "event"; items.append(d)
    items.sort(key=lambda x: x["time"], reverse=True)
    return items[:limit]


async def recent_events(limit=200, unit=None, node=None) -> list[dict]:
    q = "SELECT time, unit, node, level, msg FROM events"
    args, where = [], []
    if unit:
        args.append(unit); where.append(f"unit=${len(args)}")
    if node:
        args.append(node); where.append(f"node=${len(args)}")
    if where:
        q += " WHERE " + " AND ".join(where)
    args.append(limit)
    q += f" ORDER BY time DESC LIMIT ${len(args)}"
    async with _pool.acquire() as c:
        rows = await c.fetch(q, *args)
        return [dict(r) for r in rows]
