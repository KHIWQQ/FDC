"""
fdc_radio.py — Serial bridge between the host PC/RPi and the FDC Dongle (T-Beam #1)

Handles:
  - Plug-and-play auto-detect: active WHOAMI/IDENT handshake, probed in parallel
  - Hotplug detect (pyudev on Linux, comport polling elsewhere)
  - Send commands to the FDC dongle (fire cmd, ping, status request)
  - Parse incoming JSON lines from the dongle
  - Thread-safe event queue for the Web App
  - status_callback(event, data) for UI状态 (searching / connected / disconnected)
"""

import serial
import serial.tools.list_ports
import threading
import json
import time
import queue
import logging
import platform
import secrets
import hmac as hmac_mod
import hashlib

logger = logging.getLogger("fdc_radio")

# USB-serial adapters used by T-Beam / ESP32-S3 dongles.
# 0x303A = ESP32-S3 native USB CDC (ช่องหลักที่แนะนำ — ไม่ต้องลงไดรเวอร์บน Win10/11)
KNOWN_VIDS = {
    0x303A: "ESP32-S3 native USB CDC",
    0x10C4: "Silicon Labs CP210x",
    0x1A86: "WCH CH340 / CH9102",
    0x0403: "FTDI",
}

# Description keywords as a secondary signal when VID is unknown/missing.
DESC_HINTS = ("cp210", "ch340", "ch910", "ch9102", "uart", "esp32",
              "usb serial", "usb-serial", "wch", "silicon labs")

# ESP32-S3 native USB CDC รีเซ็ตชิปเมื่อเปิดพอร์ต → ต้องรอครอบเวลาบูท (~2s)
# และส่ง WHOAMI ซ้ำเป็นระยะ เผื่อครั้งแรกส่งตอนชิปยังบูทไม่เสร็จ
PROBE_WINDOW    = 3.5    # seconds: เปิดพอร์ตค้างไว้รอ IDENT/บูท
WHOAMI_INTERVAL = 0.3    # seconds: ส่ง WHOAMI ซ้ำทุกเท่านี้ระหว่าง probe
PROBE_BAUD      = 115200

# เฟิร์มแวร์เก่า (ก่อน v3) ไม่มี WHOAMI — จดจำจาก JSON ที่มันพ่นเอง
# ยอมรับเฉพาะเมื่อไม่ได้ตั้ง psk (ตั้ง psk = บังคับยืนยันตัวตนเท่านั้น)
LEGACY_TYPES = {"FDC_GPS", "GUN_STATUS", "GUN_ACK", "PONG", "INFO", "TX_OK"}


class FdcRadio:
    BAUD = 115200
    RECONNECT_DELAY = 1.0     # was 3.0 — เร่งให้เสียบแล้วต่อเร็ว
    MAX_QUEUE = 200

    def __init__(self, port: str = None, psk: str = None):
        self.port    = port               # current/forced port
        self._forced = port is not None   # explicit port → skip auto-detect
        self._ser    = None
        self._lock   = threading.Lock()
        self._rxq    = queue.Queue(maxsize=self.MAX_QUEUE)
        self._stop   = threading.Event()
        self._wake   = threading.Event()  # hotplug/rescan → reconnect immediately
        self._thread = None
        self._hotplug_thread = None
        self.connected = False
        self.node  = None                 # identified node id of the connected dongle
        self.freq  = None                 # LoRa frequency reported by the dongle (MHz)
        self.fw    = None                 # firmware version ("v3" หรือ "legacy")
        self.nodes = {}                   # device → node (multi-dongle map from last scan)
        self._pending_ser = None          # open port handed over from a successful probe
        self.status_callback = None       # fn(event:str, data:dict)
        self._quiet_search = False        # กัน log "ไม่เจอพอร์ต" รัวทุกวินาที
        # PSK (hex 32 chars = 16 bytes, same key as the dongle's loraKey).
        # When set, IDENT must carry a valid HMAC-SHA256(psk, nonce) —
        # อุปกรณ์ที่แค่เลียน JSON ตอบ IDENT เฉยๆ จะถูกปฏิเสธ
        self.psk = None
        if psk:
            try:
                self.psk = bytes.fromhex(psk)
                if len(self.psk) != 16:
                    raise ValueError("psk must be 16 bytes (32 hex chars)")
            except ValueError as e:
                logger.error(f"Invalid radio psk ({e}) — dongle authentication DISABLED")
                self.psk = None

    # ------------------------------------------------------------------
    #  Lifecycle
    # ------------------------------------------------------------------

    def start(self):
        """Start background RX + hotplug threads (auto-detects port if not forced)."""
        self._stop.clear()
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()
        self._hotplug_thread = threading.Thread(target=self._hotplug_loop, daemon=True)
        self._hotplug_thread.start()
        logger.info("FdcRadio started")

    def stop(self):
        self._stop.set()
        self._wake.set()
        if self._ser and self._ser.is_open:
            try: self._ser.close()
            except Exception: pass
        logger.info("FdcRadio stopped")

    def rescan(self) -> bool:
        """Force a fresh auto-detect (drops the current link if any)."""
        logger.info("Manual rescan requested")
        self._quiet_search = False
        if self.connected:
            self._drop_connection("rescan")
        self._wake.set()
        return True

    def status(self) -> dict:
        return {
            "connected": self.connected,
            "port":      self.port if self.connected else None,
            "node":      self.node if self.connected else None,
            "freq":      self.freq if self.connected else None,
            "fw":        self.fw   if self.connected else None,
            "status":    "connected" if self.connected else "searching",
        }

    # ------------------------------------------------------------------
    #  Auto-detect (active probe, parallel)
    # ------------------------------------------------------------------

    @staticmethod
    def _open_serial(device: str, baud: int, timeout: float) -> serial.Serial:
        """Open a port WITHOUT asserting DTR/RTS, so we don't auto-reset the ESP32.

        exclusive=True กันสองโปรเซสเปิดพอร์ตเดียวกันชนกัน (Linux/Mac)
        """
        ser = serial.Serial()
        ser.port     = device
        ser.baudrate = baud
        ser.timeout  = timeout
        ser.dtr      = False
        ser.rts      = False
        if platform.system() != "Windows":
            ser.exclusive = True
        ser.open()
        return ser

    def _candidate_ports(self) -> list[str]:
        """All serial ports that *could* be our dongle, by VID/PID then description."""
        cands = []
        for p in serial.tools.list_ports.comports():
            desc = (p.description or "").lower()
            if p.vid in KNOWN_VIDS or any(h in desc for h in DESC_HINTS):
                cands.append(p.device)
        # de-dup, preserve order
        seen, out = set(), []
        for d in cands:
            if d not in seen:
                seen.add(d)
                out.append(d)
        return out

    def _probe_port(self, device: str, found: dict, lock: threading.Lock):
        """Open one candidate, ask WHOAMI (+nonce) repeatedly, wait for IDENT.

        - เปิดพอร์ตอาจรีเซ็ต ESP32-S3 (USB CDC) → รอครอบเวลาบูทใน PROBE_WINDOW
          และส่ง WHOAMI ซ้ำทุก WHOAMI_INTERVAL จนกว่าชิปจะตื่นมาตอบ
        - psk ตั้งไว้ = challenge-response: ต้องตอบ hmac ที่ถูกต้องเท่านั้น
        - psk ไม่ตั้ง + เจอ JSON ของระบบเรา (เฟิร์มแวร์เก่าไม่มี WHOAMI)
          → ยอมรับเป็น legacy dongle
        - พอร์ตที่ผ่านการยืนยัน จะถูก "เปิดค้างไว้" ส่งต่อให้ _connect ใช้เลย
          (ปิดแล้วเปิดใหม่ = ชิปรีเซ็ตอีกรอบ เสียเวลาบูทฟรีๆ)
        """
        try:
            ser = self._open_serial(device, PROBE_BAUD, timeout=0.2)
        except Exception as e:
            logger.debug(f"Probe open failed {device}: {e}")
            return
        accepted = False
        legacy_seen = False          # เห็น JSON ของระบบเรา (INFO/FDC_GPS) แต่ยังไม่เห็น IDENT
        try:
            time.sleep(0.1)              # let CDC settle after open
            ser.reset_input_buffer()
            nonce = secrets.token_hex(8)
            whoami = (json.dumps({"type": "WHOAMI", "nonce": nonce}) + "\n").encode()
            deadline = time.time() + PROBE_WINDOW
            last_who = 0.0
            while time.time() < deadline:
                if time.time() - last_who >= WHOAMI_INTERVAL:
                    try:
                        ser.write(whoami)
                        ser.flush()
                    except Exception:
                        return
                    last_who = time.time()
                line = ser.readline()
                if not line:
                    continue
                try:
                    msg = json.loads(line.decode("utf-8", errors="replace").strip())
                except (json.JSONDecodeError, ValueError):
                    continue
                mtype = msg.get("type")

                # IDENT ชนะเสมอ — v3 พ่น INFO/FDC_GPS ปนมาด้วย ห้ามรีบสรุป legacy ก่อนเห็น IDENT
                if mtype == "IDENT" and msg.get("role") == "FDC_DONGLE":
                    if self.psk:
                        expect = hmac_mod.new(self.psk, nonce.encode(),
                                              hashlib.sha256).hexdigest()[:32]
                        got = msg.get("hmac") or ""
                        if not hmac_mod.compare_digest(got, expect):
                            # boot-burst IDENT ไม่มี hmac ก็ตกเคสนี้ — รอคำตอบที่ถูก challenge
                            logger.debug(f"{device}: IDENT without valid hmac — ignored")
                            continue
                    node = int(msg.get("node", 1))
                    with lock:
                        found[device] = {"node": node,
                                         "freq": msg.get("freq"),
                                         "fw":   msg.get("fw"),
                                         "ser":  ser}
                    accepted = True
                    logger.info(f"IDENT confirmed on {device}: node={node} "
                                f"fw={msg.get('fw')} freq={msg.get('freq')} "
                                f"auth={'HMAC' if self.psk else 'none'}")
                    return

                # เฟิร์มแวร์เก่า: ไม่รู้จัก WHOAMI แต่พ่น JSON ของระบบเรา — จดไว้ก่อน
                # แต่ยัง "ไม่" ยอมรับทันที รอจน window หมดเผื่อ IDENT ตามมา
                if mtype in LEGACY_TYPES:
                    legacy_seen = True

            # window หมดแล้วยังไม่เห็น IDENT → ถ้ามี traffic ของเรา + ไม่ตั้ง psk = legacy จริง
            if legacy_seen and not self.psk:
                with lock:
                    found[device] = {"node": 1, "freq": None,
                                     "fw": "legacy", "ser": ser}
                accepted = True
                logger.warning(f"{device}: legacy dongle (no WHOAMI) accepted "
                               f"— แนะนำ flash เฟิร์มแวร์ v3")
        except Exception as e:
            logger.debug(f"Probe error {device}: {e}")
        finally:
            if not accepted:
                try: ser.close()
                except Exception: pass

    def _find_port(self) -> str | None:
        """Probe every candidate port in parallel; return the verified dongle's device."""
        candidates = self._candidate_ports()
        if not candidates:
            # เตือนครั้งแรกครั้งเดียว — ระหว่างรอเสียบ dongle ไม่ต้อง log รัวทุกวินาที
            if not self._quiet_search:
                logger.warning("No candidate serial ports present — waiting for dongle…")
                self._quiet_search = True
            return None
        if not self._quiet_search:
            logger.info(f"Probing candidate ports: {candidates}")

        found: dict[str, dict] = {}
        lock = threading.Lock()
        threads = []
        for dev in candidates:
            t = threading.Thread(target=self._probe_port, args=(dev, found, lock), daemon=True)
            t.start()
            threads.append(t)
        for t in threads:
            t.join(PROBE_WINDOW + 1.0)

        if not found:
            if not self._quiet_search:
                logger.warning("No FDC dongle answered WHOAMI on any candidate port"
                               + (" (psk set — unauthenticated devices rejected)" if self.psk else ""))
                self._quiet_search = True
            return None

        self._quiet_search = False
        self.nodes = {dev: info["node"] for dev, info in found.items()}
        # Pick the lowest node id (deterministic with several dongles plugged in).
        device, info = sorted(found.items(), key=lambda kv: kv[1]["node"])[0]
        self.node = info["node"]
        self.freq = info.get("freq")
        self.fw   = info.get("fw")
        # ส่งต่อพอร์ตที่เปิดค้างจาก probe ให้ _connect (กัน ESP32 รีเซ็ตซ้ำ)
        self._pending_ser = info.get("ser")
        # ปิดพอร์ตของตัวที่ไม่ถูกเลือก
        for dev, inf in found.items():
            if dev != device and inf.get("ser"):
                try: inf["ser"].close()
                except Exception: pass
        logger.info(f"Selected FDC dongle: {device} (node {self.node}, "
                    f"fw={self.fw}, {self.freq} MHz); all={self.nodes}")
        return device

    # ------------------------------------------------------------------
    #  Connection management
    # ------------------------------------------------------------------

    def _connect(self) -> bool:
        if self._forced:
            port = self.port
        else:
            self._set_status("searching", {})
            port = self._find_port()
        if not port:
            return False
        try:
            # ใช้พอร์ตที่ probe เปิดค้างไว้ถ้ามี — เปิดใหม่ = ESP32-S3 รีเซ็ตอีกรอบ
            pend = self._pending_ser
            self._pending_ser = None
            if pend and pend.is_open and pend.port == port:
                pend.timeout = 1
                self._ser = pend
            else:
                if pend:
                    try: pend.close()
                    except Exception: pass
                self._ser = self._open_serial(port, self.BAUD, timeout=1)
            self.port = port
            self.connected = True
            logger.info(f"Connected to {port} @ {self.BAUD} (node {self.node}, fw={self.fw})")
            self._set_status("connected",
                             {"port": port, "node": self.node,
                              "freq": self.freq, "fw": self.fw})
            return True
        except Exception as e:
            logger.error(f"Serial open failed: {e}")
            return False

    def _drop_connection(self, reason: str = ""):
        was = self.connected
        self.connected = False
        if self._ser:
            try: self._ser.close()
            except Exception: pass
            self._ser = None
        if was:
            logger.warning(f"Connection dropped: {reason}")
            self._set_status("disconnected", {"port": self.port, "reason": reason})

    def _set_status(self, event: str, data: dict):
        if self.status_callback:
            try:
                self.status_callback(event, data)
            except Exception as e:
                logger.error(f"status_callback error: {e}")

    # ------------------------------------------------------------------
    #  RX Loop (background thread)
    # ------------------------------------------------------------------

    def _rx_loop(self):
        while not self._stop.is_set():
            if not self.connected or not self._ser or not self._ser.is_open:
                if not self._connect():
                    self._wake.wait(self.RECONNECT_DELAY)
                    self._wake.clear()
                    continue

            # Read one line. A close from another thread (unplug/rescan) surfaces
            # here as SerialException/OSError/TypeError — treat all as a disconnect.
            try:
                line = self._ser.readline()
            except (serial.SerialException, OSError, TypeError, AttributeError) as e:
                if not self._stop.is_set() and self.connected:
                    logger.warning(f"Serial error: {e}")
                self._drop_connection(str(e))
                self._wake.wait(self.RECONNECT_DELAY)
                self._wake.clear()
                continue

            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            try:
                msg = json.loads(text)
                if not self._rxq.full():
                    self._rxq.put_nowait(msg)
                else:
                    # Drop oldest
                    try: self._rxq.get_nowait()
                    except Exception: pass
                    self._rxq.put_nowait(msg)
            except json.JSONDecodeError:
                logger.debug(f"Non-JSON line: {text}")

    # ------------------------------------------------------------------
    #  Hotplug detection (A3)
    # ------------------------------------------------------------------

    def _hotplug_loop(self):
        """Linux: pyudev tty monitor (instant). Else: comport polling every 1s."""
        if platform.system() == "Linux":
            try:
                import pyudev
                ctx = pyudev.Context()
                mon = pyudev.Monitor.from_netlink(ctx)
                mon.filter_by(subsystem="tty")
                mon.start()
                logger.info("Hotplug: pyudev tty monitor active")
                for device in iter(lambda: mon.poll(timeout=1), None):
                    if self._stop.is_set():
                        return
                    if device is None:
                        continue
                    node = device.device_node
                    if device.action == "add":
                        logger.info(f"Hotplug add: {node}")
                        self._quiet_search = False
                        self._wake.set()                 # reconnect now, don't wait for poll
                    elif device.action == "remove":
                        logger.info(f"Hotplug remove: {node}")
                        if self.connected and node == self.port:
                            self._drop_connection("unplugged")
                return
            except ImportError:
                logger.info("pyudev not installed — using comport polling fallback")
            except Exception as e:
                logger.warning(f"pyudev monitor failed ({e}) — using polling fallback")
        self._poll_loop()

    def _poll_loop(self):
        try:
            prev = {p.device for p in serial.tools.list_ports.comports()}
        except Exception:
            prev = set()
        while not self._stop.is_set():
            self._stop.wait(1.0)
            if self._stop.is_set():
                break
            try:
                cur = {p.device for p in serial.tools.list_ports.comports()}
            except Exception:
                continue
            added   = cur - prev
            removed = prev - cur
            if added:
                logger.info(f"Hotplug(poll) added: {added}")
                self._quiet_search = False
                self._wake.set()
            if removed and self.connected and self.port in removed:
                self._drop_connection("unplugged")
            prev = cur

    # ------------------------------------------------------------------
    #  TX commands
    # ------------------------------------------------------------------

    def _send(self, obj: dict) -> bool:
        if not self.connected or not self._ser:
            logger.warning("Not connected — cannot send")
            return False
        with self._lock:
            try:
                line = json.dumps(obj) + "\n"
                self._ser.write(line.encode("utf-8"))
                self._ser.flush()
                return True
            except Exception as e:
                logger.error(f"Send failed: {e}")
                self._drop_connection(str(e))
                return False

    def send_fire_cmd(self, gun: int, az_mils: int, el_mils: int, charge: int) -> bool:
        """Send a fire command to a specific gun via LoRa."""
        return self._send({
            "type":   "FIRE_CMD",
            "gun":    gun,
            "az":     az_mils,
            "el":     el_mils,
            "charge": charge
        })

    def ping_gun(self, gun: int) -> bool:
        """Ping a gun to check connectivity."""
        return self._send({"type": "PING_GUN", "gun": gun})

    def request_status(self) -> bool:
        """Request latest status for all guns."""
        return self._send({"type": "STATUS_REQ"})

    # ------------------------------------------------------------------
    #  RX Queue
    # ------------------------------------------------------------------

    def get_messages(self, max_count: int = 50) -> list[dict]:
        """Drain up to max_count messages from the RX queue."""
        msgs = []
        for _ in range(max_count):
            try:
                msgs.append(self._rxq.get_nowait())
            except queue.Empty:
                break
        return msgs

    def get_message_nowait(self) -> dict | None:
        try:
            return self._rxq.get_nowait()
        except queue.Empty:
            return None
