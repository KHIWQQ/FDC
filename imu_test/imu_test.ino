/*
 * QMI8658 IMU AXIS TEST — ตัวเทสไจโรแยก (universal)  v5
 * ==============================================================
 * ตัวนี้เก็บไว้เผื่อไปเทสบอร์ด/เครื่องอื่นที่ไจโร QMI8658 "ใช้ได้"
 * → ลองหาไจโรทั้ง 2 อินเทอร์เฟซอัตโนมัติ เจอทางไหนใช้ทางนั้น:
 *     • I2C  : scan 0x6B/0x6A ทั้งบัส Wire(17/18) และ Wire1(42/41)
 *     • SPI  : CS=GPIO34, ไล่ลำดับขา SCK/MISO/MOSI จาก {35,36,37} × mode 0/3
 *   (T-Beam Supreme ตัวนี้ไจโรตาย/ไม่มี — แต่เครื่องอื่นอาจมีบน I2C หรือ SPI)
 *
 * ⚠️ ถ้าไจโรอยู่ SPI (ขา 34-37 = octal-PSRAM) → ต้องตั้ง Tools→PSRAM→Disabled
 *    ถ้าไจโรอยู่ I2C → ตั้งอะไรก็ได้ ไม่เกี่ยว
 *
 * เจอแล้ว: โชว้ ACC(g)/GYR(dps)/|A| + แถบไฮไลต์แกนเด่น (เหมือนหน้า IMU M5Stick)
 *   วางนิ่ง→ACC แกน≈±1.00g=แกนตั้งฉากพื้น (|A|≈1.00=อ่านถูก) ; หมุน→GYR แกนพุ่ง=แกนนั้น
 *   BOOT = tare ไบอัสไจโร (ตอนเจอ) / rescan (ตอนยังไม่เจอ)
 */

#include <Wire.h>
#include <XPowersLib.h>
#include <U8g2lib.h>

// ── ขา ───────────────────────────────────────────────────────────
#define I2C_SDA    17
#define I2C_SCL    18
#define I2C1_SDA   42
#define I2C1_SCL   41
#define BUTTON_PIN  0
#define IMU_CS     34               // CS ไจโร (SPI) — SCK/MISO/MOSI ไล่จาก {35,36,37}
uint8_t pSCK = 36, pMISO = 37, pMOSI = 35;

// ── QMI8658 register (เหมือนกันทั้ง I2C/SPI) ─────────────────────
#define QMI_WHOAMI    0x00          // = 0x05
#define QMI_REVISION  0x01
#define QMI_CTRL1     0x02          // 0x40 = 4-wire SPI/I2C + auto-inc + little-endian
#define QMI_CTRL2     0x03
#define QMI_CTRL3     0x04
#define QMI_CTRL5     0x06
#define QMI_CTRL7     0x08          // 0x03 = เปิด accel+gyro
#define QMI_TEMP_L    0x33
#define QMI_AX_L      0x35
#define QMI_RESET     0x60

#define QMI_CTRL2_VAL 0x23          // accel ±8g  → 4096 LSB/g
#define QMI_CTRL3_VAL 0x53          // gyro ±512dps→ 64  LSB/dps
#define QMI_ACC_LSB   4096.0f
#define QMI_GYR_LSB   64.0f
#define FW_VER "v5"

XPowersAXP2101 pmu;
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R1, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

// ── อินเทอร์เฟซที่เจอ ────────────────────────────────────────────
enum IfType { IF_NONE, IF_I2C, IF_SPI };
IfType   iface = IF_NONE;
TwoWire *i2cBus = nullptr; const char *i2cBusNm = "-"; uint8_t i2cAddr = 0;
uint8_t  spiMode = 0, spiWho0 = 0xEE, spiWho3 = 0xEE;   // WHOAMI ดิบ SPI (debug)
uint8_t  imuRev = 0;

struct IMU { float ax, ay, az, gx, gy, gz, tC; };
float gbx = 0, gby = 0, gbz = 0;
const char SPIN[4] = {'|', '/', '-', '\\'};

// ── SPI บิตแบง ──────────────────────────────────────────────────
void sckIdle() { digitalWrite(pSCK, spiMode == 3 ? HIGH : LOW); }
void applyPins() {
  pinMode(pSCK, OUTPUT); pinMode(pMOSI, OUTPUT); pinMode(pMISO, INPUT_PULLUP);
  pinMode(IMU_CS, OUTPUT); digitalWrite(IMU_CS, HIGH); sckIdle();
}
uint8_t bbXfer(uint8_t out) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    if (spiMode == 3) {
      digitalWrite(pSCK, LOW); digitalWrite(pMOSI, (out >> i) & 1); delayMicroseconds(2);
      digitalWrite(pSCK, HIGH); in = (in << 1) | (digitalRead(pMISO) & 1); delayMicroseconds(2);
    } else {
      digitalWrite(pMOSI, (out >> i) & 1); delayMicroseconds(2);
      digitalWrite(pSCK, HIGH); in = (in << 1) | (digitalRead(pMISO) & 1); delayMicroseconds(2);
      digitalWrite(pSCK, LOW);
    }
  }
  return in;
}

// ── register access — dispatch ตามอินเทอร์เฟซที่เจอ ─────────────
uint8_t imuRead8(uint8_t reg) {
  if (iface == IF_I2C) {
    i2cBus->beginTransmission(i2cAddr); i2cBus->write(reg); i2cBus->endTransmission(false);
    i2cBus->requestFrom((int)i2cAddr, 1); return i2cBus->available() ? i2cBus->read() : 0;
  }
  digitalWrite(IMU_CS, LOW); bbXfer(reg | 0x80); uint8_t v = bbXfer(0x00); digitalWrite(IMU_CS, HIGH);
  return v;
}
void imuWrite8(uint8_t reg, uint8_t val) {
  if (iface == IF_I2C) {
    i2cBus->beginTransmission(i2cAddr); i2cBus->write(reg); i2cBus->write(val); i2cBus->endTransmission(); return;
  }
  digitalWrite(IMU_CS, LOW); bbXfer(reg & 0x7F); bbXfer(val); digitalWrite(IMU_CS, HIGH);
}
void imuReadBlock(uint8_t reg, uint8_t *buf, int n) {
  if (iface == IF_I2C) {
    i2cBus->beginTransmission(i2cAddr); i2cBus->write(reg); i2cBus->endTransmission(false);
    int got = i2cBus->requestFrom((int)i2cAddr, n);
    for (int i = 0; i < n; i++) buf[i] = (i < got && i2cBus->available()) ? i2cBus->read() : 0;
    return;
  }
  digitalWrite(IMU_CS, LOW); bbXfer(reg | 0x80);
  for (int i = 0; i < n; i++) buf[i] = bbXfer(0x00);
  digitalWrite(IMU_CS, HIGH);
}

// ── หาไจโร: I2C ก่อน แล้ว SPI ────────────────────────────────────
bool probeI2C(TwoWire &bus, uint8_t a) {
  bus.beginTransmission(a); if (bus.endTransmission() != 0) return false;
  bus.beginTransmission(a); bus.write(QMI_WHOAMI); bus.endTransmission(false);
  bus.requestFrom((int)a, 1); return bus.available() && bus.read() == 0x05;
}
bool detectI2C() {
  struct { TwoWire *b; const char *nm; } B[2] = {{&Wire, "W0"}, {&Wire1, "W1"}};
  uint8_t A[2] = {0x6B, 0x6A};
  for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++)
    if (probeI2C(*B[i].b, A[j])) { iface = IF_I2C; i2cBus = B[i].b; i2cBusNm = B[i].nm; i2cAddr = A[j]; return true; }
  return false;
}
bool detectSPI() {
  iface = IF_SPI;                                 // ให้ imuRead8 ใช้เส้นทาง SPI
  uint8_t perms[6][3] = {{36,37,35},{36,35,37},{37,36,35},{37,35,36},{35,36,37},{35,37,36}};
  for (int p = 0; p < 6; p++) {
    pSCK = perms[p][0]; pMISO = perms[p][1]; pMOSI = perms[p][2]; applyPins();
    for (int m = 0; m < 2; m++) {
      spiMode = m ? 3 : 0; sckIdle(); delayMicroseconds(10);
      uint8_t w = imuRead8(QMI_WHOAMI);
      if (m == 0 && p == 0) spiWho0 = w;
      if (m == 1 && p == 0) spiWho3 = w;
      Serial.printf("  SPI SCK=%d MISO=%d MOSI=%d m%d -> 0x%02X\n", pSCK, pMISO, pMOSI, spiMode, w);
      if (w == 0x05) return true;                 // เจอ! ขา+mode ตั้งค้างไว้
    }
  }
  pSCK = 36; pMISO = 37; pMOSI = 35; spiMode = 0; applyPins();
  iface = IF_NONE; return false;
}
bool detect() { return detectI2C() || detectSPI(); }

bool imuInit() {
  imuWrite8(QMI_RESET, 0xB0); delay(20);
  if (imuRead8(QMI_WHOAMI) != 0x05) return false;
  imuRev = imuRead8(QMI_REVISION);
  imuWrite8(QMI_CTRL1, 0x40);
  imuWrite8(QMI_CTRL2, QMI_CTRL2_VAL);
  imuWrite8(QMI_CTRL3, QMI_CTRL3_VAL);
  imuWrite8(QMI_CTRL5, 0x00);
  imuWrite8(QMI_CTRL7, 0x03);
  delay(50); return true;
}
void imuRead(IMU &d) {
  uint8_t b[12]; imuReadBlock(QMI_AX_L, b, 12);
  int16_t rax=(int16_t)(b[0]|(b[1]<<8)),  ray=(int16_t)(b[2]|(b[3]<<8));
  int16_t raz=(int16_t)(b[4]|(b[5]<<8)),  rgx=(int16_t)(b[6]|(b[7]<<8));
  int16_t rgy=(int16_t)(b[8]|(b[9]<<8)),  rgz=(int16_t)(b[10]|(b[11]<<8));
  d.ax=rax/QMI_ACC_LSB; d.ay=ray/QMI_ACC_LSB; d.az=raz/QMI_ACC_LSB;
  d.gx=rgx/QMI_GYR_LSB; d.gy=rgy/QMI_GYR_LSB; d.gz=rgz/QMI_GYR_LSB;
  uint8_t t[2]; imuReadBlock(QMI_TEMP_L, t, 2); d.tC=(int16_t)(t[0]|(t[1]<<8))/256.0f;
}
void imuTare(int n = 64) {
  double sx=0,sy=0,sz=0; int c=0;
  for (int i=0;i<n;i++){ IMU d; imuRead(d); sx+=d.gx; sy+=d.gy; sz+=d.gz; c++; delay(5); }
  if (c){ gbx=sx/c; gby=sy/c; gbz=sz/c; }
  Serial.printf("[TARE] bias %.2f %.2f %.2f dps\n", gbx, gby, gbz);
}

// ── I2C scan (ไว้โชว้หน้า NOT FOUND) ─────────────────────────────
int scanBus(TwoWire &bus, uint8_t *f, int mx) {
  int n = 0;
  for (uint8_t a = 1; a < 127 && n < mx; a++) { bus.beginTransmission(a); if (bus.endTransmission() == 0) f[n++] = a; }
  return n;
}
void drawScan(int x, int y, const char *lbl, uint8_t *f, int n) {
  char s[24]; int p = snprintf(s, sizeof(s), "%s", lbl);
  if (n == 0) snprintf(s + p, sizeof(s) - p, "-");
  for (int i = 0; i < n && p < 20; i++) p += snprintf(s + p, sizeof(s) - p, "%02X ", f[i]);
  display.drawStr(x, y, s);
}
void drawNotFound(char hb) {
  uint8_t f0[12], f1[12]; int n0 = scanBus(Wire, f0, 12), n1 = scanBus(Wire1, f1, 12);
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  char buf[16]; snprintf(buf, sizeof(buf), "IMU %s", FW_VER); display.drawStr(3, 8, buf);
  buf[0] = hb; buf[1] = 0; display.drawStr(57, 8, buf);
  display.drawStr(3, 18, "NO GYRO");
  display.drawHLine(0, 21, 64);
  display.setFont(u8g2_font_5x7_tf);
  drawScan(2, 30, "W0:", f0, n0);
  drawScan(2, 39, "W1:", f1, n1);
  char s[20];
  snprintf(s, sizeof(s), "SPI34 m0:%02X", spiWho0); display.drawStr(2, 50, s);
  snprintf(s, sizeof(s), "SPI34 m3:%02X", spiWho3); display.drawStr(2, 59, s);
  display.drawHLine(0, 63, 64);
  display.drawStr(2, 72, "I2C gyro=6B/6A");
  display.drawStr(2, 81, "SPI gyro WHO=05");
  display.drawStr(2, 90, "here: none=dead");
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(3, 105, "PSRAM off");
  display.drawStr(3, 116, "= try SPI");
  display.drawStr(3, 126, "BOOT rescan");
  display.sendBuffer();
}

void setup() {
  Serial.begin(115200); delay(400);
  Serial.println("\n=== QMI8658 IMU TEST (I2C+SPI) " FW_VER " ===");
  Serial.println("*** ไจโรบน SPI ต้องตั้ง Tools->PSRAM->Disabled / บน I2C ไม่ต้อง ***");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // ไม่แตะขา SPI (34-37) ตรงนี้ — เผื่อไจโรอยู่ I2C + PSRAM เปิด จะได้ไม่ชนขา PSRAM
  // detectSPI() จะเรียก applyPins() เองเฉพาะตอนลอง SPI (หลัง I2C ไม่เจอ)

  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(); display.setFont(u8g2_font_6x10_tf);
  display.clearBuffer(); display.drawStr(3, 12, "IMU " FW_VER);
  display.drawStr(3, 28, "scan..."); display.sendBuffer();

  Wire1.begin(I2C1_SDA, I2C1_SCL);
  bool pmuOk = pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, I2C1_SDA, I2C1_SCL);
  if (pmuOk) {
    pmu.setALDO1Voltage(3300); pmu.enableALDO1(); pmu.setALDO2Voltage(3300); pmu.enableALDO2();
    pmu.setALDO3Voltage(3300); pmu.enableALDO3(); pmu.setALDO4Voltage(3300); pmu.enableALDO4();
    pmu.setBLDO1Voltage(3300); pmu.enableBLDO1(); delay(300);
  }
  Serial.printf("[PMU] %s\n", pmuOk ? "OK" : "FAIL");

  if (detect()) {
    bool ok = imuInit();
    if (iface == IF_I2C) Serial.printf("[QMI8658] I2C %s @0x%02X WHO=0x05 REV=0x%02X %s\n",
                                       i2cBusNm, i2cAddr, imuRev, ok ? "OK" : "init fail");
    else                 Serial.printf("[QMI8658] SPI SCK=%d MISO=%d MOSI=%d m%d REV=0x%02X %s\n",
                                       pSCK, pMISO, pMOSI, spiMode, imuRev, ok ? "OK" : "init fail");
    if (ok) { delay(300); imuTare(); Serial.println(">>> วางนิ่ง/หมุน เพื่อแมปแกน"); }
    else iface = IF_NONE;
  } else {
    Serial.println("[QMI8658] ไม่เจอทั้ง I2C และ SPI");
  }
}

void loop() {
  static uint32_t lastBtn = 0; static uint8_t spin = 0;

  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastBtn > 300) {
    lastBtn = millis();
    if (iface != IF_NONE) { display.clearBuffer(); display.drawStr(3, 40, "TARE..."); display.sendBuffer(); imuTare(); }
    else if (detect() && imuInit()) imuTare();
  }

  if (iface == IF_NONE) {
    drawNotFound(SPIN[spin++ & 3]); delay(800);
    if (detect()) { if (imuInit()) imuTare(); else iface = IF_NONE; }
    return;
  }

  IMU d; imuRead(d);
  float gx = d.gx - gbx, gy = d.gy - gby, gz = d.gz - gbz;
  float amag = sqrtf(d.ax*d.ax + d.ay*d.ay + d.az*d.az);

  float agx=fabsf(gx), agy=fabsf(gy), agz=fabsf(gz), gmax=fmaxf(agx,fmaxf(agy,agz));
  char hint[12];
  if (gmax > 25.0f) {
    char ax = (agx>=agy && agx>=agz)?'X':(agy>=agz?'Y':'Z');
    float v = (ax=='X')?gx:(ax=='Y')?gy:gz;
    snprintf(hint, sizeof(hint), "ROT %c%c", ax, v>=0?'+':'-');
  } else {
    float aax=fabsf(d.ax), aay=fabsf(d.ay), aaz=fabsf(d.az);
    char ax = (aax>=aay && aax>=aaz)?'X':(aay>=aaz?'Y':'Z');
    float v = (ax=='X')?d.ax:(ax=='Y')?d.ay:d.az;
    snprintf(hint, sizeof(hint), "GRV %c%c", ax, v>=0?'+':'-');
  }

  Serial.printf("A[g] X=%+5.2f Y=%+5.2f Z=%+5.2f |A|=%4.2f | G[dps] X=%+6.1f Y=%+6.1f Z=%+6.1f | T=%.1fC | %s\n",
                d.ax, d.ay, d.az, amag, gx, gy, gz, d.tC, hint);

  char buf[16];
  display.clearBuffer(); display.setFont(u8g2_font_6x10_tf);
  display.drawStr(3, 8, "IMU " FW_VER);
  buf[0] = SPIN[spin++ & 3]; buf[1] = 0; display.drawStr(57, 8, buf);
  if (iface == IF_I2C) snprintf(buf, sizeof(buf), "I2C 0x%02X", i2cAddr);
  else                 snprintf(buf, sizeof(buf), "SPI m%d", spiMode);
  display.drawStr(3, 18, buf);
  display.drawHLine(0, 21, 64);
  display.drawStr(3, 31, "ACC (g)");
  snprintf(buf, sizeof(buf), "X%+6.2f", d.ax); display.drawStr(3, 41, buf);
  snprintf(buf, sizeof(buf), "Y%+6.2f", d.ay); display.drawStr(3, 51, buf);
  snprintf(buf, sizeof(buf), "Z%+6.2f", d.az); display.drawStr(3, 61, buf);
  snprintf(buf, sizeof(buf), "|A|=%4.2f", amag); display.drawStr(3, 72, buf);
  display.drawStr(3, 83, "GYR (dps)");
  snprintf(buf, sizeof(buf), "X%+6.1f", gx); display.drawStr(3, 93, buf);
  snprintf(buf, sizeof(buf), "Y%+6.1f", gy); display.drawStr(3, 103, buf);
  snprintf(buf, sizeof(buf), "Z%+6.1f", gz); display.drawStr(3, 113, buf);
  display.drawBox(0, 116, 64, 12);
  display.setDrawColor(0); display.drawStr(3, 125, hint); display.setDrawColor(1);
  display.sendBuffer();
  delay(80);
}
