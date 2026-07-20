/*
 * RM3100 COMPASS TEST — ตัวเทสเข็มทิศแยก (สำหรับตั้งค่าตอนติดตั้ง)
 * ==============================================================
 * RM3100 = ตัวที่กำหนด "ทิศ" จริงบน gun_unit (ไจโร QMI8658 ตาย → ไม่ใช้)
 * ใช้เครื่องมือนี้ตอนติดตั้งบนแท่นปืน เพื่อ:
 *   1) รู้ว่าแกน X / Y ของเข็มทิศชี้ไปทางไหนของบอร์ด (หมุนบอร์ดดู X/Y ตอบสนอง)
 *   2) เทียบทิศกับเข็มทิศมือถือ → รู้ค่าเยื้อง (offset) ตอนติดตั้ง
 *   3) เช็คทิศทางหมุน: หมุนขวา(ตามเข็ม) → ทิศต้อง "ขึ้น" (^) → ยืนยัน HDG_REVERSE ถูก
 *   4) |H| (สนามแนวราบ) ต้อง "นิ่ง" ~20-40µT — ถ้ากระโดดใกล้เหล็กปืน = มีการรบกวนแม่เหล็ก
 *
 * ทิศที่โชว์ = สูตรเดียวกับ gun_unit เป๊ะ (atan2(x,-y)+HDG_REVERSE+declination+trim)
 * ปรับ 3 ค่าล่างให้ตรง gun_unit เพื่อทดสอบค่าติดตั้งได้
 */

#include <Wire.h>
#include <SPI.h>
#include <XPowersLib.h>
#include <U8g2lib.h>

// ── ขา (ตรงกับ gun_unit) ─────────────────────────────────────────
#define I2C_SDA    17          // OLED
#define I2C_SCL    18
#define I2C1_SDA   42          // PMU
#define I2C1_SCL   41
#define RM_CLK     39          // RM3100 SPI (HSPI)
#define RM_MISO    38
#define RM_MOSI     2
#define RM_CS      21
#define BUTTON_PIN  0

// ── ค่าจูนทิศ (ตั้งให้ตรง gun_unit เพื่อเทียบทิศจริง) ────────────
#define MAG_DECLINATION  -0.74f  // WMM-2025 ไทยกลาง 2026 (ตรงกับ gun_unit v7.2)
#define HDG_REVERSE      1     // 1 = กลับทิศหมุนให้ตรงเข็มทิศ (หมุนขวา=เพิ่ม)
#define HDG_TRIM_DEG     0.0f  // จูนติดตั้งครั้งเดียว

// ── RM3100 register ──────────────────────────────────────────────
#define RM_REG_CMM    0x01
#define RM_REG_CCX    0x04
#define RM_REG_CCY    0x06
#define RM_REG_CCZ    0x08
#define RM_REG_MX     0x24
#define RM_REG_STATUS 0x34
#define RM_REVID      0x36     // = 0x22
#define RM_CYCLE      200
#define RM_SCALE_UT   (1.0f / (0.3671f * RM_CYCLE))   // → µT (gain≈73.4 LSB/µT ที่ CC=200)

XPowersAXP2101 pmu;
SPIClass rm_spi(HSPI);
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R1, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

struct Mag { float x, y, z; };
bool magOk = false;
uint8_t revid = 0;
const char SPIN[4] = {'|', '/', '-', '\\'};

// ── RM3100 SPI helpers (เหมือน gun_unit) ────────────────────────
uint8_t rm_read(uint8_t reg) {
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(reg | 0x80);
  uint8_t v = rm_spi.transfer(0x00);
  digitalWrite(RM_CS, HIGH);
  return v;
}
void rm_write(uint8_t reg, uint8_t val) {
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(reg & 0x7F); rm_spi.transfer(val);
  digitalWrite(RM_CS, HIGH);
}
void rm_write_cc(uint8_t reg, uint16_t cc) {
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(reg & 0x7F); rm_spi.transfer((cc >> 8) & 0xFF); rm_spi.transfer(cc & 0xFF);
  digitalWrite(RM_CS, HIGH);
}
static int32_t to_s24(uint8_t a, uint8_t b, uint8_t c) {
  int32_t v = ((int32_t)a << 16) | ((int32_t)b << 8) | c;
  if (v & 0x800000) v |= 0xFF000000;
  return v;
}

bool rm_init() {
  pinMode(RM_CS, OUTPUT); digitalWrite(RM_CS, HIGH);
  rm_spi.begin(RM_CLK, RM_MISO, RM_MOSI, RM_CS);
  rm_spi.setFrequency(1000000);
  rm_spi.setDataMode(SPI_MODE0);
  delay(10);
  revid = rm_read(RM_REVID);
  Serial.printf("[RM3100] REVID=0x%02X (expect 0x22)\n", revid);
  if (revid != 0x22) return false;
  rm_write_cc(RM_REG_CCX, RM_CYCLE);
  rm_write_cc(RM_REG_CCY, RM_CYCLE);
  rm_write_cc(RM_REG_CCZ, RM_CYCLE);
  rm_write(RM_REG_CMM, 0x71);
  delay(10);
  return true;
}
bool rm_read_mag(Mag &m) {
  if (!(rm_read(RM_REG_STATUS) & 0x80)) return false;
  uint8_t b[9];
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(RM_REG_MX | 0x80);
  for (int i = 0; i < 9; i++) b[i] = rm_spi.transfer(0x00);
  digitalWrite(RM_CS, HIGH);
  m.x = to_s24(b[0], b[1], b[2]) * RM_SCALE_UT;
  m.y = to_s24(b[3], b[4], b[5]) * RM_SCALE_UT;
  m.z = to_s24(b[6], b[7], b[8]) * RM_SCALE_UT;
  return true;
}

// ── ทิศ (สูตรเดียวกับ gun_unit) ─────────────────────────────────
float headingFrom(const Mag &m) {
  float d = atan2f(m.x, -m.y) * 180.0f / PI;
  if (d < 0) d += 360.0f;
#if HDG_REVERSE
  d = 360.0f - d;
#endif
  d += MAG_DECLINATION + HDG_TRIM_DEG;
  while (d < 0) d += 360.0f;
  while (d >= 360.0f) d -= 360.0f;
  return d;
}
const char* cardinal(float d) {
  static const char* C[8] = {"N ", "NE", "E ", "SE", "S ", "SW", "W ", "NW"};
  return C[((int)((d + 22.5f) / 45.0f)) & 7];
}

void drawNoMag(char hb) {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(3, 8, "MAG TEST");
  char b[16]; b[0] = hb; b[1] = 0; display.drawStr(57, 8, b);
  display.drawHLine(0, 11, 64);
  display.drawStr(3, 28, "RM3100");
  display.drawStr(3, 40, "NOT FOUND");
  snprintf(b, sizeof(b), "REV=%02X", revid); display.drawStr(3, 58, b);
  display.drawStr(3, 70, "want 22");
  display.drawStr(3, 88, "check SPI");
  display.drawStr(3, 100, "39/38/2");
  display.drawStr(3, 112, "CS=21");
  display.drawStr(3, 126, "BOOT retry");
  display.sendBuffer();
}

void setup() {
  Serial.begin(115200); delay(400);
  Serial.println("\n=== RM3100 COMPASS TEST ===");
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(); display.setFont(u8g2_font_6x10_tf);
  display.clearBuffer(); display.drawStr(3, 12, "MAG TEST");
  display.drawStr(3, 28, "init..."); display.sendBuffer();

  Wire1.begin(I2C1_SDA, I2C1_SCL);
  bool pmuOk = pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, I2C1_SDA, I2C1_SCL);
  if (pmuOk) {
    pmu.setALDO1Voltage(3300); pmu.enableALDO1(); pmu.setALDO2Voltage(3300); pmu.enableALDO2();
    pmu.setALDO3Voltage(3300); pmu.enableALDO3(); pmu.setALDO4Voltage(3300); pmu.enableALDO4();
    pmu.setBLDO1Voltage(3300); pmu.enableBLDO1(); delay(300);
  }
  Serial.printf("[PMU] %s\n", pmuOk ? "OK" : "FAIL");

  magOk = rm_init();
  Serial.printf("[RM3100] %s\n", magOk ? "OK" : "FAIL");
  if (magOk) Serial.println(">>> หมุนบอร์ดดูแกน X/Y + ทิศ | เทียบเข็มทิศมือถือ | หมุนขวาทิศต้องขึ้น");
}

void loop() {
  static uint32_t lastBtn = 0, lastTrend = 0;
  static uint8_t spin = 0;
  static float prevHdg = 0; static char trend = '-';

  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastBtn > 300) {
    lastBtn = millis();
    if (!magOk) magOk = rm_init();
  }

  if (!magOk) { drawNoMag(SPIN[spin++ & 3]); delay(800); magOk = rm_init(); return; }

  Mag m;
  if (!rm_read_mag(m)) { delay(20); return; }

  float hmag = sqrtf(m.x * m.x + m.y * m.y);           // สนามแนวราบ (ควรนิ่ง)
  float hdg  = headingFrom(m);
  float mil  = hdg * 6400.0f / 360.0f;

  // ทิศทางหมุน (ทุก 250ms): ^ = ทิศเพิ่ม, v = ทิศลด
  if (millis() - lastTrend > 250) {
    float dH = hdg - prevHdg;
    while (dH > 180) dH -= 360; while (dH < -180) dH += 360;
    trend = (dH > 3) ? '^' : (dH < -3) ? 'v' : '-';
    prevHdg = hdg; lastTrend = millis();
  }

  Serial.printf("MAG[uT] X=%+6.1f Y=%+6.1f Z=%+6.1f |H|=%5.1f | HDG=%5.1f deg  %.0f mil  %s %c\n",
                m.x, m.y, m.z, hmag, hdg, mil, cardinal(hdg), trend);

  char buf[16];
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(3, 8, "MAG TEST");
  buf[0] = SPIN[spin++ & 3]; buf[1] = 0; display.drawStr(57, 8, buf);
  display.drawStr(3, 18, "REV 22 ok");
  display.drawHLine(0, 21, 64);
  display.drawStr(3, 31, "FIELD uT");
  snprintf(buf, sizeof(buf), "X%+6.1f", m.x); display.drawStr(3, 41, buf);
  snprintf(buf, sizeof(buf), "Y%+6.1f", m.y); display.drawStr(3, 51, buf);
  snprintf(buf, sizeof(buf), "Z%+6.1f", m.z); display.drawStr(3, 61, buf);
  snprintf(buf, sizeof(buf), "|H|%5.1f", hmag); display.drawStr(3, 72, buf);
  display.drawHLine(0, 76, 64);
  display.drawStr(3, 86, "HEADING");
  display.setFont(u8g2_font_9x15B_tf);
  snprintf(buf, sizeof(buf), "%3.0f", hdg); display.drawStr(3, 102, buf);   // องศา ตัวใหญ่
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(39, 100, "deg");
  snprintf(buf, sizeof(buf), "%4.0f mil", mil); display.drawStr(3, 114, buf);
  // แถบล่าง: ทิศหลัก + ทิศทางหมุน
  display.drawBox(0, 116, 64, 12);
  display.setDrawColor(0);
  snprintf(buf, sizeof(buf), "%s  turn %c", cardinal(hdg), trend);
  display.drawStr(3, 125, buf);
  display.setDrawColor(1);
  display.sendBuffer();
  delay(80);
}
