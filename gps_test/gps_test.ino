/*
 * GPS TEST — สเก็ตช์ทดสอบ GPS แยกเดี่ยว (บอร์ด T-Beam SUPREME / ESP32-S3)
 * ───────────────────────────────────────────────────────────────────
 * จุดประสงค์: แยกปัญหา GPS ออกจากเฟิร์มแวร์หลัก — รันเฉพาะ PMU(จ่ายไฟ)+GPS
 *   ไม่มี LoRa / เข็มทิศ RM3100 / IMU / FreeRTOS มากวน
 *   ใช้ "พิน + การจ่ายไฟ" ชุดเดียวกับ gun_unit.ino เป๊ะ → เทียบกันได้ตรง ๆ
 *
 * ⚠ สำคัญ: บอร์ดนี้ไฟเลี้ยง GPS มาจาก AXP2101 (PMU) — ต้อง init PMU + เปิดราง
 *   ALDO/BLDO ก่อน ไม่งั้นโมดูล GPS ไม่ติด (จะได้ chars=0 หลอก ๆ ว่าเป็น HW)
 *
 * วิธีใช้:
 *   1) เปิดไฟล์นี้ใน Arduino IDE (บอร์ดเดียวกับ gun_unit) แล้ว Flash
 *   2) เปิด Serial Monitor ที่ 115200
 *   3) เอาออก "ที่โล่งเห็นฟ้า" — cold start อาจใช้ 1–15 นาทีกว่าจะ fix แรก
 *
 * อ่านผล:
 *   • echo NMEA ดิบ — ดูบรรทัด $G?GSV (ดาวเทียมในสายตา/in view):
 *       - GSV ไม่มีดาวเทียมเลย  → เสาอากาศไม่รับสัญญาณ (เสาหลวม/เสีย/ในร่ม)
 *       - GSV มีดาวเทียม แต่ยังไม่ fix → cold start, รอ + อยู่ที่โล่ง
 *   • SUMMARY ทุก 5 วิ — chars / sats(ที่ใช้) / fix / พิกัด + วินิจฉัยอัตโนมัติ
 *   • OLED — sats + fix + พิกัด (พกออกสนามไม่ต้องมีคอม)
 *
 * ตั้ง ECHO_RAW เป็น 0 ถ้าอยากปิด echo ดิบ (อ่าน SUMMARY ง่ายขึ้น)
 */

#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <U8g2lib.h>

// ── พิน (ตรงกับ gun_unit.ino) ────────────────────────────────────
#define I2C_SDA     17       // OLED
#define I2C_SCL     18
#define I2C1_SDA    42       // PMU (AXP2101)
#define I2C1_SCL    41
#define GPS_RX_PIN   9       // ESP รับจาก GPS (TX ของโมดูล)
#define GPS_TX_PIN   8
#define GPS_EN_PIN   7
#define GPS_BAUD  9600

#define ECHO_RAW     1       // 1 = echo NMEA ดิบลง Serial, 0 = ปิด

TinyGPSPlus  gps;
HardwareSerial gpsSerial(1);
XPowersAXP2101 pmu;
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

bool     pmuOk = false;
uint32_t lastSummary = 0, lastOled = 0, t0 = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== GPS TEST (standalone) ===");

  // OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin();
  display.setFont(u8g2_font_6x10_tf);
  display.clearBuffer();
  display.drawStr(0, 14, "GPS TEST");
  display.drawStr(0, 30, "init PMU...");
  display.sendBuffer();

  // PMU — จ่ายไฟ GPS (เปิดรางเดียวกับ gun_unit.ino)
  Wire1.begin(I2C1_SDA, I2C1_SCL);
  if (pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, I2C1_SDA, I2C1_SCL)) {
    pmuOk = true;
    pmu.setALDO3Voltage(3300); pmu.enableALDO3();
    pmu.setALDO4Voltage(3300); pmu.enableALDO4();
    pmu.setALDO1Voltage(3300); pmu.enableALDO1();
    pmu.setALDO2Voltage(3300); pmu.enableALDO2();
    pmu.setBLDO1Voltage(3300); pmu.enableBLDO1();
    Serial.println("[PMU] OK — rails enabled (GPS powered)");
  } else {
    Serial.println("[PMU] FAIL — GPS อาจไม่ได้รับไฟ! (เช็ค Wire1 42/41)");
  }
  delay(300);

  // GPS
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);
  delay(100);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[GPS] serial @ %d baud  RX=%d TX=%d EN=%d\n",
                GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN, GPS_EN_PIN);
  Serial.println("รอข้อมูล... ดูบรรทัด $G?GSV = ดาวเทียมในสายตา");
  Serial.println("ออกที่โล่งเห็นฟ้า — cold start อาจ 1-15 นาที\n");
  t0 = millis();
}

void loop() {
  // อ่าน + (echo ดิบ) + ป้อนให้ parser
  while (gpsSerial.available()) {
    char c = gpsSerial.read();
#if ECHO_RAW
    Serial.write(c);             // เห็น $GPGGA/$GPGSV ตรง ๆ
#endif
    gps.encode(c);
  }

  // ── SUMMARY ทุก 5 วิ ──
  if (millis() - lastSummary >= 5000) {
    lastSummary = millis();
    unsigned long chars = gps.charsProcessed();
    Serial.println("\n---------- GPS SUMMARY ----------");
    Serial.printf("uptime     : %lus\n", (unsigned long)((millis() - t0) / 1000));
    Serial.printf("PMU        : %s\n", pmuOk ? "OK (powered)" : "FAIL");
    Serial.printf("chars rx   : %lu\n", chars);
    Serial.printf("csum errs  : %lu\n", (unsigned long)gps.failedChecksum());
    Serial.printf("fix sentns : %lu\n", (unsigned long)gps.sentencesWithFix());
    Serial.printf("sats (used): %d  (valid=%d)\n",
                  gps.satellites.value(), gps.satellites.isValid());
    Serial.printf("fix (loc)  : %d\n", gps.location.isValid());
    if (gps.location.isValid())
      Serial.printf("position   : %.6f, %.6f  alt=%.1fm  age=%lums\n",
                    gps.location.lat(), gps.location.lng(),
                    gps.altitude.meters(), (unsigned long)gps.location.age());
    if (gps.date.isValid() && gps.time.isValid())
      Serial.printf("UTC        : %04d-%02d-%02d %02d:%02d:%02d\n",
                    gps.date.year(), gps.date.month(), gps.date.day(),
                    gps.time.hour(), gps.time.minute(), gps.time.second());
    // วินิจฉัยอัตโนมัติ
    if (!pmuOk)
      Serial.println(">> PMU FAIL: GPS ไม่ได้รับไฟ — แก้ PMU ก่อน");
    else if (chars < 20)
      Serial.println(">> chars~0: ไม่มีข้อมูลเข้าเลย = สาย/พิน/baud/โมดูลเสีย (HW)");
    else if (!gps.location.isValid())
      Serial.println(">> มีข้อมูลแต่ยังไม่ fix = เสาอากาศ/ไม่เห็นฟ้า/cold start (รอ+ออกที่โล่ง)");
    else
      Serial.println(">> FIX OK! GPS ปกติ");
    Serial.println("---------------------------------\n");
  }

  // ── OLED ทุก 1 วิ (พกออกสนาม) ──
  if (millis() - lastOled >= 1000) {
    lastOled = millis();
    char b[40];
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, "GPS TEST");
    snprintf(b, 40, "chars %lu", (unsigned long)gps.charsProcessed());
    display.drawStr(0, 22, b);
    snprintf(b, 40, "sats %d  fix %d",
             gps.satellites.value(), (int)gps.location.isValid());
    display.drawStr(0, 34, b);
    if (gps.location.isValid()) {
      snprintf(b, 40, "%.5f", gps.location.lat()); display.drawStr(0, 46, b);
      snprintf(b, 40, "%.5f", gps.location.lng()); display.drawStr(0, 58, b);
    } else {
      display.drawStr(0, 46, gps.charsProcessed() < 20 ? "NO DATA (HW?)" : "no fix yet");
      display.drawStr(0, 58, pmuOk ? "PMU ok, wait sky" : "PMU FAIL!");
    }
    display.sendBuffer();
  }
}
