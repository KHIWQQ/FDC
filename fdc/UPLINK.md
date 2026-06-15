# Uplink ศอย. → ศูนย์บัญชาการ (MQTT over mTLS) — คู่มือใช้งานจริง

ระบบเชื่อมชั้นบนแบบ **military grade**: MQTT + mutual TLS (CNSA Suite) + HMAC anti-replay
บนคำสั่งยิง + store-and-forward เน็ตหลุดไม่หาย

> **ศอย. ทำงานออฟไลน์ได้ครบ** — ถ้าไม่เปิด uplink (ค่าเริ่มต้น) หรือไม่ได้ติดตั้ง
> `paho-mqtt`/`pyyaml` ระบบยังรับพิกัด คำนวณ และสั่งยิงในเครื่องได้ปกติ (edge autonomy)

---

## ทำไม MQTT ไม่ใช่ HTTPS (สำหรับชั้นนี้)

| | MQTT | HTTPS |
|--|------|-------|
| ศูนย์สั่งยิงลงมา (downlink push) | ทันที | ต้อง poll |
| ลิงก์หลุดบ่อย (4G/SATCOM) | persistent + auto-reconnect + QoS | เปิด-ปิดทุก request |
| เน็ตหลุดไม่หายข้อมูล | store-and-forward ในตัว | ต้องทำเอง |
| ตรวจ node ตาย | LWT อัตโนมัติ | ไม่มี |

ลิงก์อื่นในระบบ (USB serial dongle, SSE ไปเบราว์เซอร์) ของเดิมถูกแล้ว ไม่ต้องแตะ

---

## องค์ประกอบ

| ไฟล์ | หน้าที่ |
|------|--------|
| `uplink.py` | MQTT client ฝั่ง ศอย. — mTLS, anti-replay, offline queue |
| `config.yaml` | ตัวตน node + ตั้งค่า broker/TLS/คีย์ |
| `certs/fdc-pki.sh` | ออก/จัดการใบรับรอง (CA, server, node, revoke, CRL) |
| `central/sign_command.py` | ฝั่งศูนย์ — เซ็น+ส่งคำสั่งยิง downlink |
| `test/mosquitto.conf` | broker ทดสอบ mTLS ในแล็บ |

---

## Quickstart — ทดสอบเต็มระบบในแล็บ

```bash
cd fdc

# 1) ติดตั้ง dependency (ครั้งเดียว)
pip install -r requirements.txt          # paho-mqtt, pyyaml รวมแล้ว
sudo apt install mosquitto mosquitto-clients

# 2) สร้าง PKI + ใบรับรอง (ECDSA P-384 / CNSA)
cd certs
./fdc-pki.sh init                        # สร้าง CA
./fdc-pki.sh server localhost            # ใบรับรอง broker (ใช้ localhost ตอนทดสอบ)
./fdc-pki.sh node BN1-A FDC01            # ใบรับรอง ศอย.
./fdc-pki.sh hmac                        # สุ่มคีย์ HMAC → cmd_hmac.env
cd ..

# 3) เปิด broker (เทอร์มินัล 1)
mosquitto -c test/mosquitto.conf -v

# 4) เปิด ศอย. (เทอร์มินัล 2)
#    แก้ config.yaml:  uplink.enabled: true,  broker.host: localhost,
#    tls.client_cert: certs/fdc01.crt,  tls.client_key: certs/fdc01.key
source certs/cmd_hmac.env                # โหลดคีย์ HMAC
python3 main.py

# 5) ศูนย์สั่งยิงลงมา (เทอร์มินัล 3)
source certs/cmd_hmac.env                # คีย์เดียวกัน!
cd central
./sign_command.py --unit BN1-A --node FDC01 \
    --publish --broker localhost --port 8883 \
    --ca ../certs/ca.crt --cert ../certs/broker.crt --key ../certs/broker.key \
    fire_mission --tgt-lat 13.7440 --tgt-lon 100.5302 --gun 2
```

ผลที่คาดหวัง: log ของ ศอย. ขึ้น `Downlink ACCEPTED: fire_mission` แล้วยิงคำสั่งออก LoRa
ถ้าส่งซ้ำซองเดิม → `Downlink REJECTED (replayed seq …)`

---

## ชั้นความปลอดภัย (defense in depth)

```
คำสั่งยิงจากศูนย์
   │
   ├─ ชั้น 1: mutual TLS  ── broker ตรวจใบรับรอง client, client ตรวจ broker
   │                         (ใครไม่มีใบรับรองที่ CA เราออก = ต่อไม่ได้)
   │
   └─ ชั้น 2: HMAC-SHA256 + seq + ts  ── แม้ broker ถูกเจาะ ก็ปลอม/replay
                                          คำสั่งยิงไม่ได้ถ้าไม่มีคีย์ HMAC
```

- **mTLS** กันคนนอกเข้าระบบ
- **HMAC** กันคำสั่งยิงปลอม/ซ้ำ แม้ตัวกลาง (broker) ถูกยึด

---

## ปฏิบัติการ (สิ่งที่ต้องทำสม่ำเสมอ)

- **เครื่องหาย/ถูกยึด:** `./fdc-pki.sh revoke <node>` แล้วเอา `crl.pem` ไปวางที่ broker
- **ใบรับรองหมดอายุ (397 วัน):** ออกใหม่ด้วย `./fdc-pki.sh node …`
- **CRL หมดอายุ (30 วัน):** `./fdc-pki.sh crl` แล้วอัปเดตที่ broker
- **คีย์ HMAC:** หมุนเวียนเป็นระยะ ส่งให้ทั้งสองฝั่งผ่านช่องทางปลอดภัย

---

## จาก self-signed → CA ทหารจริง

โค้ด **ไม่ต้องแก้** — แค่เปลี่ยน "ใครออกไฟล์ ca.crt / *.crt / *.key":

| ระยะ | CA |
|------|-----|
| พัฒนา/ทดสอบ (ตอนนี้) | self-signed จาก `fdc-pki.sh` |
| นำร่อง 2–3 หน่วย | internal CA ของหน่วย (เช่น step-ca) |
| ระดับประเทศ | เชื่อม PKI กองทัพ |
| สั่งยิงจริงอันตรายสูง | กุญแจ CA ใน HSM |
