"""
app.py — FastAPI ของศูนย์บัญชาการ (STEP D1–D3)

  - startup: เชื่อม DB pool + seed admin + สตาร์ท MQTT ingest
  - Auth: POST /api/login → JWT ; RBAC ทุก endpoint (STEP D3)
  - REST: สถานะหน่วย/หมู่ปืน/ภารกิจยิง/เหตุการณ์ (กรองตามขอบเขตผู้ใช้)
  - WebSocket /ws: feed realtime (กรองตามขอบเขตผู้ใช้)
  - POST /api/command/...: เซ็น (HMAC) + ส่งคำสั่ง downlink — ต้องมีสิทธิ์ "สั่งยิง" + audit
  - จัดการผู้ใช้ (admin): /api/users

สิทธิ์ (RBAC): role = admin|commander|viewer × scope = national|unit|node
  viewer = ดูอย่างเดียว, commander = สั่งยิงได้ในขอบเขต, admin = + จัดการผู้ใช้
"""
import json
import asyncio
import logging
import contextlib

import io
import csv

from fastapi import (FastAPI, WebSocket, WebSocketDisconnect, HTTPException,
                     Depends, Request, Query)
from fastapi.responses import JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

import config
import db
import hub
import auth
import command
import mqtt_ingest

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("app")

app = FastAPI(title="FDC Central — National COP", version="0.4 (STEP D4)")
_ingest_task: asyncio.Task | None = None


@app.on_event("startup")
async def _startup():
    global _ingest_task
    await db.connect()
    await _seed_admin()
    _ingest_task = asyncio.create_task(mqtt_ingest.run_forever())
    logger.info("central ingest started (auth=%s)", config.AUTH_ENABLED)


@app.on_event("shutdown")
async def _shutdown():
    if _ingest_task:
        _ingest_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await _ingest_task
    await db.close()


async def _seed_admin():
    """สร้าง admin ระดับชาติให้อัตโนมัติถ้ายังไม่มีผู้ใช้เลย (ใช้ ADMIN_USER/ADMIN_PASSWORD)."""
    if not config.AUTH_ENABLED:
        return
    if await db.count_users() > 0:
        return
    if not config.ADMIN_PASSWORD:
        logger.warning("ยังไม่มีผู้ใช้ และไม่ได้ตั้ง ADMIN_PASSWORD — "
                       "ตั้งค่าแล้วรีสตาร์ทเพื่อสร้าง admin")
        return
    await db.create_user(config.ADMIN_USER, auth.hash_password(config.ADMIN_PASSWORD),
                         "ผู้ดูแลระบบ", "admin", "national")
    logger.info("seeded admin user '%s' (national)", config.ADMIN_USER)


def _client_ip(request: Request) -> str:
    fwd = request.headers.get("x-forwarded-for")
    return fwd.split(",")[0].strip() if fwd else (request.client.host if request.client else "?")


# ------------------------------------------------------------
#  Auth
# ------------------------------------------------------------

class LoginBody(BaseModel):
    username: str
    password: str


@app.post("/api/login")
async def login(body: LoginBody, request: Request):
    ip = _client_ip(request)
    if not config.AUTH_ENABLED:
        # dev: ออก token ของ dev-admin โดยไม่ตรวจรหัส (ใช้เฉพาะตอน AUTH_ENABLED=false)
        return {"token": "", "user": auth._DEV_USER}
    u = await db.get_user(body.username)
    if not u or u["disabled"] or not auth.verify_password(body.password, u["pw_hash"]):
        await db.insert_audit(body.username, "login", "fail", ip=ip)
        raise HTTPException(401, "ชื่อผู้ใช้หรือรหัสผ่านไม่ถูกต้อง")
    await db.set_last_login(body.username)
    await db.insert_audit(body.username, "login", "ok", ip=ip)
    user = {k: u[k] for k in ("username", "name", "role", "scope_level",
                              "scope_unit", "scope_node")}
    return {"token": auth.make_token(user), "user": user}


@app.get("/api/me")
async def me(user: dict = Depends(auth.current_user)):
    return user


# ------------------------------------------------------------
#  REST — สถานะ / ประวัติ (กรองตามขอบเขต RBAC)
# ------------------------------------------------------------

@app.get("/api/health")
async def health():
    return {
        "ok": True,
        "auth": config.AUTH_ENABLED,
        "mqtt_connected": hub.mqtt_client is not None,
        "ws_clients": hub.broadcaster.count,
    }


@app.get("/api/nodes")
async def nodes(user: dict = Depends(auth.current_user)):
    rows = await db.list_nodes()
    return [r for r in rows if auth.can_view(user, r["unit"], r["node"])]


@app.get("/api/nodes/{unit}/{node}/guns")
async def node_guns(unit: str, node: str, user: dict = Depends(auth.current_user)):
    if not auth.can_view(user, unit, node):
        raise HTTPException(403, "ไม่มีสิทธิ์ดูหน่วยนี้")
    return await db.latest_guns(unit, node)


def _scoped_filter(user: dict, unit: str | None, node: str | None):
    """บังคับขอบเขต: ผู้ใช้ที่ไม่ใช่ national จะถูกล็อก unit/node เป็นของตนเสมอ."""
    lvl = user.get("scope_level")
    if lvl == "national":
        return unit, node                      # เลือกกรองเองได้
    if lvl == "unit":
        return user.get("scope_unit"), node    # ล็อกหน่วย
    return user.get("scope_unit"), user.get("scope_node")  # ล็อกทั้งหน่วยและ node


@app.get("/api/fire_missions")
async def fire_missions(limit: int = 100, unit: str | None = None,
                        node: str | None = None,
                        user: dict = Depends(auth.current_user)):
    unit, node = _scoped_filter(user, unit, node)
    return await db.recent_fire_missions(min(limit, 1000), unit, node)


@app.get("/api/events")
async def events(limit: int = 200, unit: str | None = None,
                 node: str | None = None,
                 user: dict = Depends(auth.current_user)):
    unit, node = _scoped_filter(user, unit, node)
    return await db.recent_events(min(limit, 1000), unit, node)


@app.get("/api/audit")
async def audit(limit: int = 200, user: dict = Depends(auth.require_admin)):
    return await db.recent_audit(limit)


# ------------------------------------------------------------
#  รายงาน / AAR (STEP D4) — กรองตามขอบเขต RBAC
# ------------------------------------------------------------

@app.get("/api/reports/summary")
async def report_summary(unit: str | None = None, node: str | None = None,
                         frm: str | None = Query(None, alias="from"),
                         to: str | None = None,
                         user: dict = Depends(auth.current_user)):
    unit, node = _scoped_filter(user, unit, node)
    return {
        "scope": {"unit": unit, "node": node, "from": frm, "to": to},
        "fire_missions": await db.fire_mission_stats(unit, node, frm, to),
        "events": await db.event_stats(unit, node, frm, to),
    }


@app.get("/api/reports/aar/{unit}/{node}")
async def report_aar(unit: str, node: str, request: Request,
                     frm: str | None = Query(None, alias="from"),
                     to: str | None = None, format: str = "json",
                     user: dict = Depends(auth.current_user)):
    if not auth.can_view(user, unit, node):
        raise HTTPException(403, "ไม่มีสิทธิ์ดูหน่วยนี้")
    items = await db.aar_timeline(unit, node, frm, to)
    if format == "csv":
        await db.insert_audit(user["username"], "report:aar_export", "ok",
                              unit, node, _client_ip(request),
                              {"from": frm, "to": to, "rows": len(items)})
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["time", "kind", "gun_id", "tgt_lat", "tgt_lon",
                    "range_m", "az_mils", "el_mils", "charge", "level", "msg"])
        for it in items:
            w.writerow([it.get("time"), it.get("kind"), it.get("gun_id"),
                        it.get("tgt_lat"), it.get("tgt_lon"), it.get("range_m"),
                        it.get("az_mils"), it.get("el_mils"), it.get("charge"),
                        it.get("level"), it.get("msg")])
        fn = f"AAR_{unit}_{node}.csv".replace("/", "-")
        return Response(buf.getvalue(), media_type="text/csv",
                        headers={"Content-Disposition": f'attachment; filename="{fn}"'})
    return {"unit": unit, "node": node, "from": frm, "to": to, "items": items}


# ------------------------------------------------------------
#  คำสั่ง downlink (ศูนย์ → ศอย.) — ต้องมีสิทธิ์ "สั่งยิง" + audit
# ------------------------------------------------------------

class FireMissionCmd(BaseModel):
    tgt_lat: float
    tgt_lon: float
    gun: str = "1"
    charge: int | None = None


class GunCmd(BaseModel):
    gun: str = "1"


async def _publish_cmd(user, request, unit, node, cmd_type, data):
    ip = _client_ip(request)
    # 1) สิทธิ์: ต้องสั่งการได้ + อยู่ในขอบเขต
    if not auth.can_fire(user, unit, node):
        await db.insert_audit(user["username"], f"cmd:{cmd_type}", "denied",
                              unit, node, ip, data)
        raise HTTPException(403, "ไม่มีสิทธิ์สั่งการหน่วยนี้ (ต้องเป็น commander/admin ในขอบเขต)")
    # 2) broker พร้อมหรือไม่
    if hub.mqtt_client is None:
        await db.insert_audit(user["username"], f"cmd:{cmd_type}", "no_broker",
                              unit, node, ip, data)
        raise HTTPException(503, "MQTT broker ไม่เชื่อมต่อ — ส่งคำสั่งไม่ได้ตอนนี้")
    # 3) เซ็น + ส่ง
    try:
        env = await command.build_signed(unit, node, cmd_type, data)
    except PermissionError as e:
        await db.insert_audit(user["username"], f"cmd:{cmd_type}", "no_key",
                              unit, node, ip, data)
        raise HTTPException(403, str(e))
    topic = command.topic_for(unit, node, cmd_type)
    await hub.mqtt_client.publish(topic, json.dumps(env, separators=(",", ":")), qos=1)
    await db.insert_audit(user["username"], f"cmd:{cmd_type}", "sent", unit, node, ip,
                          {**data, "seq": env["seq"]})
    logger.info(f"downlink → {topic} seq={env['seq']} by {user['username']}")
    return {"sent": True, "topic": topic, "seq": env["seq"], "ts": env["ts"]}


@app.post("/api/command/{unit}/{node}/fire_mission")
async def cmd_fire_mission(unit: str, node: str, body: FireMissionCmd,
                           request: Request, user: dict = Depends(auth.current_user)):
    data = {"tgt_lat": body.tgt_lat, "tgt_lon": body.tgt_lon, "gun": body.gun}
    if body.charge is not None:
        data["charge"] = body.charge
    return await _publish_cmd(user, request, unit, node, "fire_mission", data)


@app.post("/api/command/{unit}/{node}/ping")
async def cmd_ping(unit: str, node: str, body: GunCmd,
                   request: Request, user: dict = Depends(auth.current_user)):
    return await _publish_cmd(user, request, unit, node, "ping", {"gun": body.gun})


@app.post("/api/command/{unit}/{node}/rescan")
async def cmd_rescan(unit: str, node: str, request: Request,
                     user: dict = Depends(auth.current_user)):
    return await _publish_cmd(user, request, unit, node, "rescan", {})


# ------------------------------------------------------------
#  จัดการผู้ใช้ (admin เท่านั้น)
# ------------------------------------------------------------

class NewUser(BaseModel):
    username: str
    password: str
    name: str | None = None
    role: str = "viewer"
    scope_level: str = "node"
    scope_unit: str | None = None
    scope_node: str | None = None


@app.get("/api/users")
async def users(admin: dict = Depends(auth.require_admin)):
    return await db.list_users()


@app.post("/api/users")
async def add_user(body: NewUser, request: Request,
                   admin: dict = Depends(auth.require_admin)):
    if body.role not in auth.ROLES:
        raise HTTPException(400, f"role ต้องเป็นหนึ่งใน {auth.ROLES}")
    if body.scope_level not in auth.SCOPES:
        raise HTTPException(400, f"scope_level ต้องเป็นหนึ่งใน {auth.SCOPES}")
    if body.scope_level in ("unit", "node") and not body.scope_unit:
        raise HTTPException(400, "scope_level unit/node ต้องระบุ scope_unit")
    if body.scope_level == "node" and not body.scope_node:
        raise HTTPException(400, "scope_level node ต้องระบุ scope_node")
    if await db.get_user(body.username):
        raise HTTPException(409, "มีผู้ใช้นี้อยู่แล้ว")
    await db.create_user(body.username, auth.hash_password(body.password), body.name,
                         body.role, body.scope_level, body.scope_unit, body.scope_node)
    await db.insert_audit(admin["username"], "user:create", "ok", ip=_client_ip(request),
                          detail={"username": body.username, "role": body.role,
                                  "scope_level": body.scope_level})
    return {"created": body.username}


@app.post("/api/users/{username}/disabled")
async def set_disabled(username: str, request: Request, disabled: bool = True,
                       admin: dict = Depends(auth.require_admin)):
    if username == admin["username"]:
        raise HTTPException(400, "ปิดบัญชีตัวเองไม่ได้")
    if not await db.get_user(username):
        raise HTTPException(404, "ไม่พบผู้ใช้")
    await db.set_user_disabled(username, disabled)
    await db.insert_audit(admin["username"], "user:disabled", "ok", ip=_client_ip(request),
                          detail={"username": username, "disabled": disabled})
    return {"username": username, "disabled": disabled}


# ------------------------------------------------------------
#  WebSocket realtime feed (กรองตามขอบเขตผู้ใช้)
# ------------------------------------------------------------

@app.websocket("/ws")
async def ws(websocket: WebSocket):
    # auth ผ่าน query param ?token=... (WebSocket ส่ง header ไม่ได้สะดวก)
    user = auth.user_from_token(websocket.query_params.get("token"))
    if user is None:
        await websocket.close(code=4401)      # 4401 = unauthorized (custom)
        return
    await websocket.accept()
    q = await hub.broadcaster.subscribe()
    try:
        # snapshot เริ่มต้น — กรองเฉพาะหน่วยที่ผู้ใช้มีสิทธิ์เห็น
        snap = [r for r in await db.list_nodes() if auth.can_view(user, r["unit"], r["node"])]
        await websocket.send_json({"kind": "snapshot", "nodes": snap})
        while True:
            msg = await q.get()                # dict ดิบจาก broadcaster
            if not auth.can_view(user, msg.get("unit"), msg.get("node")):
                continue                       # นอกขอบเขต → ไม่ส่งให้ผู้ใช้นี้
            await websocket.send_text(json.dumps(msg, default=str))
    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.debug(f"ws closed: {e}")
    finally:
        await hub.broadcaster.unsubscribe(q)


@app.exception_handler(Exception)
async def _unhandled(request, exc):
    logger.error(f"unhandled: {exc}")
    return JSONResponse(status_code=500, content={"error": str(exc)})


# ------------------------------------------------------------
#  เสิร์ฟ dashboard (STEP D2) ที่ "/" — ต้อง mount หลังนิยาม route ทุกตัว
#  เพื่อให้ /api/* และ /ws จับคู่ก่อน (specific ก่อน catch-all)
# ------------------------------------------------------------
if config.WEB_DIR:
    app.mount("/", StaticFiles(directory=config.WEB_DIR, html=True), name="web")
    logger.info(f"dashboard served from {config.WEB_DIR}")
else:
    logger.warning("WEB_DIR not found — dashboard not served (API only)")
