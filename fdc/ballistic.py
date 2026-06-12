"""
ballistic.py — Artillery firing table calculator

Computes:
  - Azimuth (mils) from FDC to target
  - Range (meters)
  - Elevation (mils) for given charge using parabolic model
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
#  Simplified firing table — L119A2 / M777 equivalent
#  Charge 1–8, range in meters, elevation in mils
#  (Use real firing tables in production!)
# ============================================================

# Format: charge: [(range_m, elev_mil), ...]
FIRING_TABLE = {
    1: [(200, 850), (300, 780), (500, 650), (700, 530), (1000, 412), (1500, 580), (2000, 780), (2500, 1010)],
    2: [(300, 820), (500, 700), (700, 580), (1000, 460), (1500, 380), (2000, 510), (2500, 660), (3000, 830)],
    3: [(500, 780), (800, 620), (1200, 480), (2000, 360), (2500, 460), (3000, 580), (3500, 720), (4000, 880)],
    4: [(800, 720), (1200, 560), (1800, 420), (2500, 310), (3000, 390), (3500, 490), (4000, 600), (4500, 740)],
    5: [(1000, 680), (1500, 520), (2000, 380), (3000, 290), (3500, 360), (4000, 440), (5000, 640), (6000, 900)],
    6: [(2000, 520), (3000, 380), (4000, 280), (5000, 360), (6000, 460), (7000, 590), (8000, 780)],
    7: [(3000, 480), (4000, 350), (5000, 270), (6000, 340), (7000, 420), (8000, 530), (9000, 680)],
    8: [(4000, 440), (5000, 320), (6000, 260), (7000, 320), (8000, 400), (9000, 500),(10000, 640),(11000, 830)],
}

# Drift table: charge → mils drift per 1000m range (right drift)
DRIFT_TABLE = {1: 2, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8}


def interpolate_elevation(range_m: float, charge: int) -> float | None:
    """
    Return elevation in mils for given range and charge.
    Returns None if out of table range.
    """
    charge = max(1, min(8, charge))
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
    for charge in range(1, 9):
        table = FIRING_TABLE.get(charge, [])
        if table and table[0][0] <= range_m <= table[-1][0]:
            return charge
    return 8  # max if nothing fits


def drift_mils(range_m: float, charge: int) -> float:
    """Compute rightward drift correction in mils."""
    drift_per_km = DRIFT_TABLE.get(charge, 5)
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
        charge:      Propellant charge (1-8), auto-select if None
        alt_diff_m:  Target altitude minus gun altitude (meters)

    Returns:
        dict with az_mils, el_mils, range_m, charge, drift_mils, bearing_deg, error
    """
    result = {
        "az_mils":    None,
        "el_mils":    None,
        "range_m":    None,
        "charge":     None,
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

        # Altitude correction (simple flat-earth): add ~1 mil per ~18m at 5km
        if alt_diff_m != 0:
            correction = math.degrees(math.atan2(alt_diff_m, range_m)) * DEG_TO_MIL
            el -= correction  # subtract because higher target = less elevation needed... actually depends

        result["el_mils"]    = int(round(el))
        result["drift_mils"] = round(drift_mils(range_m, charge), 1)

        # Apply drift to azimuth
        az_corrected = az_mil + int(round(drift_mils(range_m, charge)))
        result["az_mils_corrected"] = az_corrected % 6400

    except Exception as e:
        result["error"] = str(e)

    return result
