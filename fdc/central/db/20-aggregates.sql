-- ============================================================
--  STEP D4 — continuous aggregates (TimescaleDB)
--  สรุป telemetry/ภารกิจ ล่วงหน้า → รายงานเร็วแม้ข้อมูลดิบเยอะ
--  รันหลัง 10-schema.sql (ตั้งชื่อ 20-... ให้ลำดับถูก)
-- ============================================================

-- ภารกิจยิงรายวัน ต่อหน่วย/หมู่ปืน
CREATE MATERIALIZED VIEW IF NOT EXISTS fire_missions_daily
WITH (timescaledb.continuous) AS
SELECT time_bucket('1 day', time) AS day,
       unit, node, gun_id,
       count(*)        AS missions,
       avg(range_m)    AS avg_range_m,
       avg(charge)     AS avg_charge
FROM fire_missions
GROUP BY day, unit, node, gun_id
WITH NO DATA;

SELECT add_continuous_aggregate_policy('fire_missions_daily',
    start_offset      => INTERVAL '30 days',
    end_offset        => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour',
    if_not_exists     => TRUE);

-- เหตุการณ์รายวัน แยกตามระดับ (info/error) — ดูแนวโน้มความผิดปกติ
CREATE MATERIALIZED VIEW IF NOT EXISTS events_daily
WITH (timescaledb.continuous) AS
SELECT time_bucket('1 day', time) AS day,
       unit, node, level,
       count(*) AS n
FROM events
GROUP BY day, unit, node, level
WITH NO DATA;

SELECT add_continuous_aggregate_policy('events_daily',
    start_offset      => INTERVAL '30 days',
    end_offset        => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour',
    if_not_exists     => TRUE);

-- แบตหมู่ปืนต่ำสุดรายชั่วโมง — เฝ้าระวังพลังงานในสนาม
CREATE MATERIALIZED VIEW IF NOT EXISTS gun_battery_hourly
WITH (timescaledb.continuous) AS
SELECT time_bucket('1 hour', time) AS hour,
       unit, node, gun_id,
       min(bat) AS bat_min,
       avg(bat) AS bat_avg
FROM gun_status
GROUP BY hour, unit, node, gun_id
WITH NO DATA;

SELECT add_continuous_aggregate_policy('gun_battery_hourly',
    start_offset      => INTERVAL '7 days',
    end_offset        => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour',
    if_not_exists     => TRUE);
