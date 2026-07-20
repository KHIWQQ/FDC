"""
ballistic.py — Artillery firing table calculator

Computes:
  - Azimuth (mils) from FDC to target
  - Range (meters)
  - Elevation (mils) for given charge from a demo M101 firing table (low angle)
  - Angle of site (mils) for the gun/target altitude difference
  - Drift correction

Uses military mils: 6400 mils = 360°
"""

import math

# 1 degree = 6400/360 mils
DEG_TO_MIL = 6400.0 / 360.0
MIL_TO_DEG = 360.0 / 6400.0


# ============================================================
#  Coordinate math
# ============================================================

def haversine_range(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Return distance in meters between two WGS-84 coords."""
    R = 6371000.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlam/2)**2
    return R * 2 * math.asin(math.sqrt(a))


def azimuth_degrees(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Return bearing in degrees (0–360) from point 1 to point 2."""
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dlam = math.radians(lon2 - lon1)
    x = math.sin(dlam) * math.cos(phi2)
    y = math.cos(phi1)*math.sin(phi2) - math.sin(phi1)*math.cos(phi2)*math.cos(dlam)
    bearing = math.degrees(math.atan2(x, y))
    return (bearing + 360) % 360


def azimuth_mils(lat1: float, lon1: float, lat2: float, lon2: float) -> int:
    """Return bearing in mils (0–6399)."""
    return int(round(azimuth_degrees(lat1, lon1, lat2, lon2) * DEG_TO_MIL)) % 6400


# ============================================================
#  ตารางยิงจำลอง (DEMO) — ปืนใหญ่เบา 105 มม. M101/M101A1 (แบบ 101)
#  ลูก HE M1 · เครื่องดินส่ง (charge) 1–7 · วิถีราบ/มุมต่ำ (low angle)
#  ระยะ = เมตร · มุมยก = มิล NATO (6400)
#
#  ⚠️  ค่าสาธิต — *ไม่ใช่* ค่ายิงจริง ห้ามนำไปใช้ยิงจริง
#      เป็นค่าจำลองที่จัดให้ "สอดคล้องฟิสิกส์": มุมยกเพิ่มทางเดียวตามระยะ
#      (low angle) และดินส่งสูงยิงไกลกว่าที่มุมต่ำกว่า — เพื่อสาธิต prototype
#      ตารางยิงจริง (เทียบ FT 105-H-7) + วิถีโด่ง/มุมสูง (high angle) +
#      คอลัมน์เต็ม (เวลาวิถี, ชนวน, comp site, PE) → ทำใน ROADMAP STEP B4
# ============================================================

# Format: charge: [(range_m, elev_mil), ...]  — low angle, มุมยกเพิ่มทางเดียว
FIRING_TABLE = {
    1: [(400, 40), (800, 90), (1200, 150), (1600, 220), (2000, 300), (2400, 400), (2800, 520), (3100, 640), (3400, 790)],
    2: [(600, 45), (1000, 80), (1500, 130), (2000, 195), (2500, 270), (3000, 360), (3500, 470), (4000, 610), (4400, 780)],
    3: [(800, 45), (1200, 70), (2000, 140), (2800, 230), (3500, 330), (4200, 450), (4800, 580), (5300, 710), (5600, 790)],
    4: [(1000, 45), (1500, 65), (2500, 130), (3500, 215), (4500, 320), (5200, 410), (5800, 510), (6300, 620), (6800, 770)],
    5: [(1200, 40), (2000, 60), (3000, 110), (4000, 175), (5000, 255), (6000, 350), (6800, 450), (7500, 580), (8100, 760)],
    6: [(1500, 40), (2500, 55), (3500, 95), (4500, 150), (5500, 215), (6500, 295), (7500, 400), (8500, 540), (9400, 760)],
    7: [(2000, 40), (3000, 50), (4500, 100), (6000, 165), (7000, 220), (8000, 295), (9000, 390), (10000, 520), (11000, 770)],
}

# Drift table (DEMO): charge → มิล drift ขวา ต่อระยะ 1000 m  (M101 เกลียวขวา → drift ขวา)
# ค่าจำลอง ปรับขนาดให้สมจริง (~3–18 มิล ที่ระยะไกลสุดของแต่ละดินส่ง)
DRIFT_TABLE = {1: 1.0, 2: 1.1, 3: 1.2, 4: 1.3, 5: 1.4, 6: 1.5, 7: 1.6}


def interpolate_elevation(range_m: float, charge: int) -> float | None:
    """
    Return elevation in mils for given range and charge.
    Returns None if out of table range.
    """
    charge = max(1, min(7, charge))
    table = FIRING_TABLE.get(charge, [])
    if not table:
        return None

    # Below minimum range
    if range_m < table[0][0]:
        return None
    # Above maximum range
    if range_m > table[-1][0]:
        return None

    # Linear interpolation
    for i in range(len(table) - 1):
        r0, e0 = table[i]
        r1, e1 = table[i+1]
        if r0 <= range_m <= r1:
            t = (range_m - r0) / (r1 - r0)
            return e0 + t * (e1 - e0)

    return None


def best_charge(range_m: float) -> int:
    """Suggest best charge for given range (lowest charge that covers the range)."""
    for charge in range(1, 8):
        table = FIRING_TABLE.get(charge, [])
        if table and table[0][0] <= range_m <= table[-1][0]:
            return charge
    return 7  # max if nothing fits


def drift_mils(range_m: float, charge: int) -> float:
    """Compute rightward drift correction in mils."""
    drift_per_km = DRIFT_TABLE.get(charge, 1.3)
    return drift_per_km * (range_m / 1000.0)


# ============================================================
#  Main fire mission solver
# ============================================================

def solve_fire_mission(
    fdc_lat: float, fdc_lon: float,
    gun_lat: float, gun_lon: float,
    tgt_lat: float, tgt_lon: float,
    charge: int = None,
    alt_diff_m: float = 0.0
) -> dict:
    """
    Compute complete fire solution.

    Args:
        fdc_lat/lon: FDC (observer) position
        gun_lat/lon: Gun position
        tgt_lat/lon: Target position
        charge:      Propellant charge (1-7), auto-select if None
        alt_diff_m:  Target altitude minus gun altitude (meters)

    Returns:
        dict with az_mils, el_mils, range_m, charge, drift_mils, bearing_deg, error
    """
    result = {
        "az_mils":    None,
        "el_mils":    None,
        "range_m":    None,
        "charge":     None,
        "site_mils":  None,
        "drift_mils": None,
        "bearing_deg": None,
        "error":      None
    }

    try:
        # Range and bearing from gun to target
        range_m = haversine_range(gun_lat, gun_lon, tgt_lat, tgt_lon)
        az_deg  = azimuth_degrees(gun_lat, gun_lon, tgt_lat, tgt_lon)
        az_mil  = int(round(az_deg * DEG_TO_MIL)) % 6400

        result["range_m"]     = round(range_m, 1)
        result["bearing_deg"] = round(az_deg, 2)
        result["az_mils"]     = az_mil

        # Auto-select charge if not specified
        if charge is None:
            charge = best_charge(range_m)
        result["charge"] = charge

        # Elevation
        el = interpolate_elevation(range_m, charge)
        if el is None:
            result["error"] = f"Range {range_m:.0f}m out of table for charge {charge}"
            return result

        # Angle of site (มุมที่ตั้ง) — ชดเชยความต่างระดับความสูงระหว่างปืนกับเป้าหมาย
        #   มุมยิงจริง (QE) = elevation(ระยะ) + angle of site
        #   alt_diff_m = ความสูงเป้า − ความสูงปืน (เมตร)
        #     เป้า "สูงกว่า" ปืน (alt_diff_m > 0) → site เป็นบวก → "เพิ่ม" มุมยิง (ยกลำกล้องสูงขึ้น)
        #     เป้า "ต่ำกว่า" ปืน (alt_diff_m < 0) → site เป็นลบ → "ลด" มุมยิง
        #   (โมเดลเรขาคณิตอย่างง่าย ยังไม่รวม complementary angle of site)
        site_mils = 0.0
        if alt_diff_m:
            site_mils = math.degrees(math.atan2(alt_diff_m, range_m)) * DEG_TO_MIL
            el += site_mils

        result["el_mils"]    = int(round(el))
        result["site_mils"]  = round(site_mils, 1)
        drift = drift_mils(range_m, charge)
        result["drift_mils"] = round(drift, 1)

        # Apply drift to azimuth (ใช้ค่า drift เดียวกับที่รายงาน — คำนวณครั้งเดียว)
        az_corrected = az_mil + int(round(drift))
        result["az_mils_corrected"] = az_corrected % 6400

    except Exception as e:
        result["error"] = str(e)

    return result
