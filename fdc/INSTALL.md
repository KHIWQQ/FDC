# การติดตั้ง / รัน FDC Web App (ศอย.)

เสียบ dongle (T-Beam ตัวแม่ข่าย) เข้าพอร์ต USB เครื่องไหนก็ได้ แล้วรันโปรแกรม —
ระบบจะ **ตรวจหา + เชื่อมต่อให้เองภายใน ~1 วินาที** (active WHOAMI/IDENT handshake)
และเปิดเบราว์เซอร์ไปที่ `http://localhost:5000` อัตโนมัติ

---

## วิธีที่ 1 — รันด้วยสคริปต์ (ต้องมี Python 3.10+)

| ระบบ | คำสั่ง |
|------|--------|
| Linux / Mac | `./run.sh` |
| Windows | ดับเบิลคลิก `run.bat` |

ครั้งแรกจะสร้าง virtualenv (`.venv/`) และติดตั้ง dependency จาก `requirements.txt` ให้เอง
ครั้งต่อไปจะเริ่มทันที

## วิธีที่ 2 — ไฟล์รันไฟล์เดียว (ไม่ต้องมี Python บนเครื่องเป้าหมาย)

บนเครื่องที่มี Python:

```bash
./build.sh           # Linux/Mac  -> dist/fdc-server
# Windows: รัน build.sh ผ่าน Git Bash  -> dist\fdc-server.exe
```

คัดลอก `dist/fdc-server` (หรือ `.exe`) ไปเครื่องเป้าหมายแล้วดับเบิลคลิกได้เลย
templates ถูกฝังในไฟล์เดียวแล้ว

> ปิดการเปิดเบราว์เซอร์อัตโนมัติได้ด้วยตัวแปรแวดล้อม `FDC_NO_BROWSER=1`

---

## ไดรเวอร์ USB-serial

**แนะนำ:** ใช้ช่อง **USB CDC ในตัว ESP32-S3** (VID `303a`) — **ไม่ต้องลงไดรเวอร์** บน
Windows 10/11, Linux, Mac จะขึ้นพอร์ตให้เอง

ถ้า dongle ใช้ชิปแปลง USB-serial แยก ต้องลงไดรเวอร์ตามชิป:

| ชิป | VID | ไดรเวอร์ |
|-----|-----|----------|
| Silicon Labs CP210x | `10c4` | [CP210x VCP Driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) (Windows) |
| WCH CH340 / CH9102 | `1a86` | [CH34x driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html) (Windows) |
| FTDI | `0403` | [FTDI VCP](https://ftdichip.com/drivers/vcp-drivers/) (Windows) |

Linux/Mac โดยทั่วไปมีไดรเวอร์เหล่านี้ในเคอร์เนลอยู่แล้ว

### Linux — สิทธิ์เข้าถึงพอร์ต (ไม่ต้อง sudo)

```bash
sudo cp 99-fdc.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

หรือเพิ่ม user เข้ากลุ่ม `dialout` แล้ว logout/login: `sudo usermod -aG dialout $USER`

> ติดตั้ง `pyudev` (อยู่ใน requirements.txt บน Linux อยู่แล้ว) จะได้ hotplug แบบทันที
> ถ้าไม่มีจะ fallback เป็น polling ทุก 1 วินาที

---

## การใช้งานในเว็บ

- แถบบนสุดแสดงสถานะวิทยุ: `RADIO: CONNECTED (พอร์ต / node N / ความถี่)` / `SEARCHING…` / `DISCONNECTED`
- ปุ่ม **⟳ RESCAN** สั่งค้นหา dongle ใหม่ (เผื่อสลับพอร์ต/อุปกรณ์)

---

## ความถี่ + ความปลอดภัย LoRa (STEP B1/B2)

### ความถี่ — AS923 ไทย

เฟิร์มแวร์ v3 ใช้ย่าน **920–925 MHz** ตามประกาศ กสทช. (≤500mW EIRP ไม่ต้องขอใบอนุญาต)
มีตารางช่อง CH0–CH7 (ห่าง 600 kHz) สำหรับวางแผนความถี่หลายหน่วยไม่ให้ชนกัน:

| CH | MHz | CH | MHz |
|----|------|----|------|
| 0 | 920.6 | 4 | 923.0 (ค่าเริ่มต้น) |
| 1 | 921.2 | 5 | 923.6 |
| 2 | 921.8 | 6 | 924.2 |
| 3 | 922.4 | 7 | 924.8 |

- ตั้ง `LORA_CHANNEL` ใน `.ino` **ทั้ง dongle และ gun unit ให้ตรงกัน** แล้ว flash พร้อมกัน
- เสาอากาศ 868 MHz เดิมใช้ทดสอบได้ (ระยะหดลงเล็กน้อย) — ใช้งานจริงเปลี่ยนเป็นเสา 915/923 MHz

### การเข้ารหัส — AES-128-CCM + anti-replay

ทุกแพ็กเก็ต LoRa ถูกเข้ารหัส + ใส่ MAC (CCM tag 8B) + counter กัน replay
อุปกรณ์ทุกตัวในกองร้อยต้องมี **key เดียวกัน** (16 ไบต์)

**ตั้ง key ก่อนใช้งานจริง** (ค่า default ใช้ทดสอบเท่านั้น):

```bash
# 1. สุ่ม key ใหม่
openssl rand -hex 16          # ได้ hex 32 ตัว เช่น a1b2c3...

# 2. ส่งเข้าอุปกรณ์ทีละตัว (dongle + gun unit ทุกตัว) ผ่าน USB serial 115200:
#    พิมพ์บรรทัดนี้ลง serial monitor แล้ว enter
{"type":"SET_KEY","key":"<hex32>"}
#    ตอบ {"type":"KEY_OK"} (dongle) หรือ [KEY] OK (gun unit) = สำเร็จ (เก็บใน NVS ถาวร)

# 3. ใส่ key เดียวกันใน fdc/config.yaml → radio.psk
#    เพื่อเปิด challenge-response ตอน auto-detect (กัน dongle ปลอม)
```

> **อย่า commit key จริงลง git** — เก็บใน config.yaml เฉพาะเครื่องใช้งาน
