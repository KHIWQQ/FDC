"""
fdc_radio.py — Serial bridge between RPi and FDC Dongle (T-Beam #1)

Handles:
  - Auto-detect USB serial port
  - Send commands to FDC dongle (fire cmd, ping, status request)
  - Parse incoming JSON lines from dongle
  - Thread-safe event queue for Web App
"""

import serial
import serial.tools.list_ports
import threading
import json
import time
import queue
import logging

logger = logging.getLogger("fdc_radio")


class FdcRadio:
    BAUD = 115200
    RECONNECT_DELAY = 3.0
    MAX_QUEUE = 200

    def __init__(self, port: str = None):
        self.port    = port
        self._ser    = None
        self._lock   = threading.Lock()
        self._rxq    = queue.Queue(maxsize=self.MAX_QUEUE)
        self._stop   = threading.Event()
        self._thread = None
        self.connected = False

    # ------------------------------------------------------------------
    #  Connection management
    # ------------------------------------------------------------------

    def start(self):
        """Start background RX thread (auto-detects port if not given)."""
        self._stop.clear()
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()
        logger.info("FdcRadio started")

    def stop(self):
        self._stop.set()
        if self._ser and self._ser.is_open:
            self._ser.close()
        logger.info("FdcRadio stopped")

    def _find_port(self) -> str | None:
        """Auto-detect T-Beam SUPREME on USB (CP210x or CDC)."""
        candidates = []
        for p in serial.tools.list_ports.comports():
            desc = (p.description or "").lower()
            vid  = p.vid
            if vid in (0x10C4, 0x1A86, 0x303A):  # CP210x, CH340, ESP32-S3 CDC
                candidates.append(p.device)
            elif "cp210" in desc or "uart" in desc or "esp32" in desc:
                candidates.append(p.device)
        if candidates:
            logger.info(f"Found serial ports: {candidates}")
            return candidates[0]
        return None

    def _connect(self) -> bool:
        port = self.port or self._find_port()
        if not port:
            logger.warning("No serial port found")
            return False
        try:
            self._ser = serial.Serial(port, self.BAUD, timeout=1)
            self.connected = True
            logger.info(f"Connected to {port} @ {self.BAUD}")
            return True
        except Exception as e:
            logger.error(f"Serial open failed: {e}")
            return False

    # ------------------------------------------------------------------
    #  RX Loop (background thread)
    # ------------------------------------------------------------------

    def _rx_loop(self):
        while not self._stop.is_set():
            if not self.connected or not self._ser or not self._ser.is_open:
                if not self._connect():
                    time.sleep(self.RECONNECT_DELAY)
                    continue

            try:
                line = self._ser.readline()
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
                        except: pass
                        self._rxq.put_nowait(msg)
                except json.JSONDecodeError:
                    logger.debug(f"Non-JSON line: {text}")
            except serial.SerialException as e:
                logger.warning(f"Serial error: {e}")
                self.connected = False
                if self._ser:
                    try: self._ser.close()
                    except: pass
                time.sleep(self.RECONNECT_DELAY)
            except Exception as e:
                logger.error(f"RX loop error: {e}")
                time.sleep(0.5)

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
                self.connected = False
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
