"""
auth.py — Authentication (JWT) + RBAC (STEP D3)

โมเดลสิทธิ์: role × ขอบเขต (echelon)
  role:        admin | commander | viewer
  scope_level: national | unit | node
แยก "ดู" (viewer) ออกจาก "สั่งยิง" (commander/admin) อย่างชัดเจน
ทุกคำสั่งจะถูกบันทึก audit (ในชั้น app)

โหมด dev: AUTH_ENABLED=false → ทุก request ถือเป็น admin/national (ข้ามการตรวจ)
"""
import time
import logging

import jwt
import bcrypt
from fastapi import Depends, HTTPException, Request
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials

import config

logger = logging.getLogger("auth")

ROLES = ("admin", "commander", "viewer")
SCOPES = ("national", "unit", "node")

# ผู้ใช้สมมุติเมื่อปิด auth (dev) — เห็น/สั่งได้ทุกอย่าง
_DEV_USER = {
    "username": "dev", "name": "DEV (auth disabled)", "role": "admin",
    "scope_level": "national", "scope_unit": None, "scope_node": None,
}

_bearer = HTTPBearer(auto_error=False)


# ------------------------------------------------------------
#  รหัสผ่าน (bcrypt)
# ------------------------------------------------------------

def hash_password(pw: str) -> str:
    return bcrypt.hashpw(pw.encode("utf-8"), bcrypt.gensalt()).decode("utf-8")


def verify_password(pw: str, pw_hash: str) -> bool:
    try:
        return bcrypt.checkpw(pw.encode("utf-8"), pw_hash.encode("utf-8"))
    except Exception:
        return False


# ------------------------------------------------------------
#  JWT
# ------------------------------------------------------------

def make_token(user: dict) -> str:
    now = int(time.time())
    payload = {
        "sub": user["username"],
        "role": user["role"],
        "scope_level": user["scope_level"],
        "scope_unit": user.get("scope_unit"),
        "scope_node": user.get("scope_node"),
        "name": user.get("name"),
        "iat": now,
        "exp": now + config.JWT_TTL_MIN * 60,
    }
    return jwt.encode(payload, config.JWT_SECRET, algorithm=config.JWT_ALG)


def decode_token(token: str) -> dict:
    """คืน user dict จาก token; โยน ValueError ถ้าไม่ถูกต้อง/หมดอายุ"""
    try:
        p = jwt.decode(token, config.JWT_SECRET, algorithms=[config.JWT_ALG])
    except jwt.ExpiredSignatureError:
        raise ValueError("token หมดอายุ")
    except jwt.InvalidTokenError as e:
        raise ValueError(f"token ไม่ถูกต้อง: {e}")
    return {
        "username": p.get("sub"), "role": p.get("role"),
        "scope_level": p.get("scope_level"), "scope_unit": p.get("scope_unit"),
        "scope_node": p.get("scope_node"), "name": p.get("name"),
    }


# ------------------------------------------------------------
#  RBAC — ขอบเขตการมองเห็น / สิทธิ์สั่งการ
# ------------------------------------------------------------

def can_view(user: dict, unit: str | None, node: str | None) -> bool:
    lvl = user.get("scope_level")
    if lvl == "national":
        return True
    if lvl == "unit":
        return unit == user.get("scope_unit")
    if lvl == "node":
        return unit == user.get("scope_unit") and node == user.get("scope_node")
    return False


def can_fire(user: dict, unit: str | None, node: str | None) -> bool:
    """สั่งยิงได้เมื่อ: มีบทบาทสั่งการ (admin/commander) และอยู่ในขอบเขตที่ดูแล"""
    return user.get("role") in ("admin", "commander") and can_view(user, unit, node)


def is_admin(user: dict) -> bool:
    return user.get("role") == "admin"


# ------------------------------------------------------------
#  FastAPI dependencies
# ------------------------------------------------------------

async def current_user(
    request: Request,
    cred: HTTPAuthorizationCredentials | None = Depends(_bearer),
) -> dict:
    if not config.AUTH_ENABLED:
        return dict(_DEV_USER)
    if cred is None or not cred.credentials:
        raise HTTPException(401, "ต้องเข้าสู่ระบบ (ไม่มี bearer token)")
    try:
        user = decode_token(cred.credentials)
    except ValueError as e:
        raise HTTPException(401, str(e))
    if not user.get("username"):
        raise HTTPException(401, "token ไม่สมบูรณ์")
    return user


def user_from_token(token: str | None) -> dict | None:
    """สำหรับ WebSocket (ไม่มี header) — รับ token จาก query string"""
    if not config.AUTH_ENABLED:
        return dict(_DEV_USER)
    if not token:
        return None
    try:
        return decode_token(token)
    except ValueError:
        return None


def require_admin(user: dict = Depends(current_user)) -> dict:
    if not is_admin(user):
        raise HTTPException(403, "ต้องเป็นผู้ดูแลระบบ (admin)")
    return user
