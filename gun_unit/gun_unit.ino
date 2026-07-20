/*
 * Artillery FCS — Phase 2
 * GUN UNIT firmware v7.2 (T-Beam SUPREME)
 * v7.2 (แก้อาการ "เปิดเครื่องแล้วทุกอย่างหน่วง/ค้าง" + เข็มทิศแม่นขึ้น):
 *   - SF12/62.5 → SF9/125: airtime STATUS 3.6s → 0.25s (ต้องแฟลช fdc_dongle คู่กัน!)
 *   - ส่ง LoRa แบบไม่ทิ้ง GPS (startTransmit + อ่าน GPS ระหว่างรอ TX-done, RadioLib ≥6.0)
 *   - radio.begin() ล้มเหลว: ขึ้นจอ "RADIO FAIL" + retry (เดิมวนตายเงียบ จอค้าง Init...)
 *   - โหมด CAL 360°: BOOT ≥6s หรือ {"type":"CAL"} → หมุนปืนช้า ๆ 1 รอบ → fit วงกลม/วงรี
 *     hard/soft-iron ลง NVS ครั้งเดียวจบ (ไม่ใช่ cal ต่อเนื่องแบบที่เคยทำทิศค้าง) → ตั้งเหนือใหม่
 *   - declination -0.74° (WMM-2025 ไทยกลาง) + EMA ทิศ + สเกล µT ตรงดาต้าชีต
 * v7.3: auto-lay แม่นขึ้น (ยังอัตโนมัติ 100% เหมือนเดิม): ล็อกครั้งแรกต้องมี 2 หน้าต่าง
 *   ให้ค่าตรงกัน ≤3°, ถ่วงน้ำหนักหน้าต่างตามความเร็ว (ยิ่งเร็ว COG ยิ่งแม่น), หน้าต่าง 4s
 * v7.4: อุดช่อง "ทิศค้าง/เหนือปลอม" + cal รับวงรีเอียง:
 *   - hdgValid กลับ true เฉพาะเมื่อคำนวณทิศจากเฟรมจริงหลังรีอาร์ม (เดิมเฟรม baseline ก็ปลด
 *     valid → จอ/STATUS ออกอากาศทิศเก่าเป็น valid ได้ชั่วขณะ) + EMA เริ่มใหม่ ไม่ไหลจากค่าค้าง
 *   - Set North (ปุ่ม/SET_AZ) ต้องทิศสด — เข็มทิศหลุดอยู่ขึ้น "SET N FAIL" (เดิมรับเงียบ ๆ
 *     ด้วยค่าค้าง = ฝังเหนือผิดลง NVS) และ auto-lay หยุดจับคู่ COG กับทิศค้างตอนเซนเซอร์หลุด
 *   - CAL_CLEAR ล้าง lay+EMA เหมือนตอน CAL OK (เดิมจอยัง NSET ทั้งที่ฐานทิศเปลี่ยนแล้ว)
 *   - ล็อก auto-lay ครั้งแรกเซฟ NVS เสมอ (เดิมติดเกณฑ์ >1° — offset เล็กหายตอนรีบูต)
 *   - CAL 360°: ฟิตวงรีเต็มมีเทอมไขว้ (รับวงรีเอียง — เดิมวงรีเอียง 45° ถูกมองเป็นวงกลม
 *     = ไม่แก้อะไรเลย) + guard ความเบี้ยววัดตามแกนวงรีจริง; cal เก่าใน NVS ใช้ต่อได้
 * FreeRTOS dual-core architecture:
 *   Core 0: GPS + LoRa RX/TX + Status broadcast
 *   Core 1: Compass (RM3100) + Display + Button
 * RM3100 = เซนเซอร์แม่นในตัว (ไม่ต้องปรับ gain) — ใช้ทิศ atan2 ดิบ ไม่มี cal ต่อเนื่อง
 *   hard-iron คงที่จาก "แท่นปืน/โลหะ" ถูกดูดด้วย layOffset ตอนตั้งทิศเหนือ
 * ตั้งทิศเหนือ (หลัก): ขับเข้าที่ตั้งวิ่งตรง → auto-lay จาก GPS course (ไม่ต้องกดอะไร)
 * BUTTON (GPIO0/BOOT): ปล่อยหลังกดค้าง ≥2s = ตั้งทิศปัจจุบันเป็นเหนือ (ปากกระบอก = 0 มิล)
 * Serial/วิทยุ: {"type":"SET_AZ","deg":N} ตั้งทิศด้วยมือ / {"type":"SET_KEY",...} ตั้งคีย์
 */

#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SPI.h>
#include <XPowersLib.h>
#include <Preferences.h>
#include <mbedtls/ccm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ── Pin Definitions ──────────────────────────────────────────────
#define RADIO_SCLK_PIN   12
#define RADIO_MISO_PIN   13
#define RADIO_MOSI_PIN   11
#define RADIO_CS_PIN     10
#define RADIO_RST_PIN     5
#define RADIO_DIO1_PIN    1
#define RADIO_BUSY_PIN    4
#define I2C_SDA          17
#define I2C_SCL          18
#define I2C1_SDA         42
#define I2C1_SCL         41
#define GPS_RX_PIN        9
#define GPS_TX_PIN        8
#define GPS_EN_PIN        7
#define GPS_BAUD       9600
#define BUTTON_PIN        0
#define RM_CLK           39
#define RM_MISO          38
#define RM_MOSI           2
#define RM_CS            21   // เดิม 6 = ชนกับ GPS_PPS_PIN (GPS ขับสายค้าง CS ยกไม่ขึ้น → REVID=0x00) — ต้องย้ายสาย CS มาขา 21 ด้วย

// ── LoRa Config — ย่าน AS923 ไทย (กสทช. 920–925 MHz) ────────────
// STEP B1: ตารางช่อง — ต้องตั้ง LORA_CHANNEL ตรงกับ fdc_dongle ของกองร้อยเดียวกัน
static const float LORA_CHANNELS[] = {
  920.6, 921.2, 921.8, 922.4, 923.0, 923.6, 924.2, 924.8   // CH0–CH7, ห่าง 600kHz
};
#define LORA_CHANNEL    4          // CH4 = 923.0 MHz (กลางย่าน)
#define LORA_FREQ     (LORA_CHANNELS[LORA_CHANNEL])
#define LORA_BW       125.0     // v7.2: 62.5→125 คู่ SF9 — airtime ลด ~14 เท่า (ช่องห่าง 600kHz ยังพอ)
#define LORA_SF         9       // v7.2: 12→9 — SF12 ทำ Core 0 บล็อกส่ง ~66% ของเวลา = ทุกอย่างหน่วง
                                // *** ต้องแฟลช fdc_dongle ที่ SF/BW เดียวกัน ไม่งั้นลิงก์เงียบสนิท ***
#define LORA_CR         5
#define LORA_SYNC    0xAB
#define LORA_POWER     22
#define LORA_PREAMBLE   8
#define AZ_THRESHOLD    15
#define COMPASS_LOST_MS 1500      // อ่าน RM3100 ไม่ได้นานเกินนี้ = เข็มทิศหลุด → จอเตือน ไม่ค้างเลขเก่า
#define COMPASS_REARM_MS 1000     // หลุดนานเกิน LOST แล้วยังไม่กลับ = ลองรีอาร์ม RM3100 ซ้ำทุกเท่านี้ (กันต้องรีบูต)
#define COMPASS_STUCK_MS 2000     // อ่านได้แต่ค่าไม่ขยับเลยเกินนี้ = CMM ไม่เดิน (cold boot) → ห้ามใช้ทิศ + รีอาร์ม
#define SET_NORTH_HOLD_MS 2000    // กด BOOT ค้าง ≥2s แล้วปล่อย = ตั้งทิศปัจจุบันเป็นเหนือ (manual lay)
// ค่าเบี่ยงเข็มทิศ (declination) magnetic→true north — WMM-2025 ไทยกลาง (ลพบุรี) ปี 2026 = -0.74°
// (เหนือแม่เหล็กเบนตะวันตก 0.74°; auto-lay/SET_AZ ดูดค่านี้อยู่แล้ว — มีผลเฉพาะโหมดยังไม่ lay)
#define MAG_DECLINATION  -0.74f
// ทิศหมุน: ถ้าเลข "สลับด้าน" กับ iPhone (หมุนขวาแล้วเลขลด) ตั้ง 1 เพื่อกลับด้าน
#define HDG_REVERSE   1
// จูนชดเชยการติดตั้ง (เครื่อง↔ลำกล้อง) ครั้งเดียว — แทน set-north (องศา, +/-)
#define HDG_TRIM_DEG  0.0f
// EMA เวกเตอร์ทิศ (wrap-safe) — ตัดเลขสั่น: 0.15 ≈ เฉลี่ย ~13 เฟรม หน่วง ~0.3s (เลขมิลละเอียดกว่า
// องศา 17.8 เท่า มือสั่นนิดเดียวก็เห็นเลขวิ่ง — ถ้ารู้สึกตามการหมุนช้าไปค่อยขยับกลับทาง 0.25)
#define HDG_SMOOTH_A  0.15f
// ── โหมดคาลิเบรต hard/soft-iron ครั้งเดียวจบ: BOOT ≥6s (หรือ serial {"type":"CAL"}) แล้ว
//    หมุนปืนช้า ๆ 1 รอบเต็ม → fit วงกลม (Kåsa) + วงรีเต็มมีเทอมไขว้ (v7.4 รับวงรีเอียง) → NVS
//    (ต่างจาก cal ต่อเนื่องที่ถอดทิ้ง 2026-06-29 — อันนั้นขยับ cal ใต้ทิศตอนหันจนค้าง)
#define CAL_HOLD_MS     6000      // กดค้างถึงนี่ = โหมด CAL (สั้นกว่า = SET NORTH เดิม)
#define CAL_TIMEOUT_MS 120000     // หมุนไม่ครบใน 2 นาที = FAIL ใช้ค่าเดิม
#define CAL_BINS          36      // แบ่งรอบทิศเป็นช่อง 10° เช็คความครอบคลุม
#define CAL_MIN_BINS      33      // ต้องครอบคลุม ≥330° จึงยอมรับ
#define CAL_MIN_PTS      200      // จุดขั้นต่ำ (หมุน 1 รอบ ~60s เก็บได้ ~500 จุด)
#define CAL_MAX_PTS      720      // เพดาน buffer (720×8B ≈ 5.8KB)

// ── ตั้งทิศเหนืออัตโนมัติจากเส้นทางวิ่ง GPS (auto-lay จาก course-over-ground) ─
// ขับ/ลากปืนเข้าที่ตั้งแบบวิ่งตรงสั้น ๆ → GPS ให้ "ทิศจริง" (true north) จับคู่กับ
// เข็มทิศขณะนั้น → offset.  ทิศจริง = ทิศเข็มทิศ + layOffset (ดูด declination +
// มุมติดตั้งทิ้งหมดในก้อนเดียว) จากนั้นหมุนกระบอก ไจโร/เข็มทิศรักษาทิศเอง
#define LAY_MIN_KMH      5.0f     // ต้องวิ่งเร็วกว่านี้ COG ถึงเชื่อถือได้ (ช้า=ทิศมั่ว)
#define LAY_WINDOW_MS    4000     // v7.3: 2.5→4s ต่อหน้าต่าง — เฉลี่ยลึกขึ้น ค่านิ่งขึ้น
#define LAY_MIN_SAMPLES  8        // และต้องได้ตัวอย่าง GPS อย่างน้อยเท่านี้ในหน้าต่าง
#define LAY_R_MIN        0.985f   // ความนิ่งของทิศ (resultant length 0–1) — วิ่งตรงไม่เลี้ยว
// v7.3: ล็อกครั้งแรกต้องมี 2 หน้าต่างติดกันให้ค่าตรงกัน — กันหน้าต่างเดียวเพี้ยนฝังค่าผิดเป็น NSET
#define LAY_AGREE_DEG    3.0f     // สองหน้าต่างต่างกันไม่เกินนี้จึงยอมล็อกครั้งแรก
// v7.3: ถ่วงน้ำหนักหน้าต่างตามความเร็วเฉลี่ย (ยิ่งเร็ว COG ยิ่งแม่น) — แทน low-pass คงที่เดิม
#define LAY_W_SPD_CAP   30.0f     // ความเร็ว (กม./ชม.) ที่นับน้ำหนักเต็ม — เกินนี้แม่นพอ ๆ กัน
#define LAY_W_MAX      300.0f     // เพดานน้ำหนักสะสม — กันค่าแข็งจนดึงไม่ได้ตอนย้ายการติดตั้ง
#define LAY_ALPHA_MIN    0.10f    // ขอบเขตอัตราดึงเข้า (หลักฐานสะสมเยอะ=ดึงช้า, น้อย=ดึงไว)
#define LAY_ALPHA_MAX    0.50f
#define LAY_SAVE_MS      60000UL  // เซฟ NVS อย่างมากทุก 1 นาที (ถนอมแฟลช)

// ── RM3100 Registers ─────────────────────────────────────────────
#define RM3100_REG_CMM    0x01
#define RM3100_REG_CCX    0x04
#define RM3100_REG_CCY    0x06
#define RM3100_REG_CCZ    0x08
#define RM3100_REG_MX     0x24
#define RM3100_REG_STATUS 0x34
#define RM3100_REVID      0x36
#define RM3100_CYCLE      200
// gain ตามดาต้าชีต ~เชิงเส้น 0.3671×CC ≈ 73.4 LSB/µT ที่ CC=200 → ค่าออกมาเป็น µT จริง
// (|H| เมืองไทย ~41µT; เดิมหารด้วย sqrt(CC) ทำให้ debug |xy| โตเกินจริง ~14 เท่า — ทิศไม่เพี้ยน
//  เพราะ atan2 ใช้อัตราส่วน แต่ต้องแก้ให้ตรงก่อนใช้ |H| เป็นตัวเช็คการรบกวนจากเหล็ก)
#define RM3100_SCALE      (1.0f / (0.3671f * RM3100_CYCLE))

// ── RM3100 struct (must be before functions) ─────────────────────
struct RM3100Data { float x, y, z; };

// ── Shared State (protected by mutex) ───────────────────────────
SemaphoreHandle_t xMutex;
SemaphoreHandle_t wire1Mutex;   // กันชนบัส Wire1 (PMU/RTC, Core 0) — BME ย้ายไป Wire แล้ว, IMU ถอดแล้ว
SemaphoreHandle_t prefsMutex;   // กัน NVS (prefs) ชนข้ามคอร์: Core 0 เขียน loractr/lorakey ↔ Core 1 เขียน layOff

struct SharedState {
  // GPS
  double  lat = 0, lon = 0;
  float   alt = 0;
  uint8_t sats = 0;
  uint8_t satsView = 0;       // ดาวเทียมในสายตา (GSV) — ดูว่าเสาอากาศรับสัญญาณได้ไหม
  bool    gpsValid = false;
  // COG/ความเร็ว (Core 0 ตั้งจาก GPS RMC, Core 1 ใช้ตั้งทิศเหนืออัตโนมัติ)
  float   cog = 0;            // course over ground (°true) — ทิศการเคลื่อนที่จริง
  float   speedKmh = 0;
  bool    cogValid = false;
  // Heading
  float   hdgDeg = 0, hdgMils = 0;
  bool    hdgValid = false;
  // Aim command
  int     azMils = 0, elMils = 0, charge = 1;
  uint8_t aimSeq = 0;
  bool    hasCmd = false;
  uint32_t cmdTime = 0;
  // Battery
  uint8_t battPct = 0;
  // Link to FDC (Core 0 ตั้งตอนรับแพ็กเก็ตจริง, Core 1 แสดงผล)
  uint32_t lastFdcRx = 0;     // millis() ที่ได้ยินแม่ข่ายล่าสุด (0 = ยังไม่เคย)
  int16_t  lastRssi  = 0;     // RSSI ของแพ็กเก็ตแม่ข่ายล่าสุด (dBm)
  // Meteorology (BME280) — ส่งให้ ศอย. ชดเชยความหนาแน่นอากาศในการคำนวณยิง
  float    temp = 0, press = 0, hum = 0;
  bool     metValid = false;
  // ตั้งทิศด้วยมือผ่าน SET_AZ (Core 0 รับคำสั่ง → Core 1 คำนวณ lay)
  bool    setAzRequest = false;       // ตั้งทิศด้วยมือ: ลำกล้องชี้ setAzDeg อยู่
  float   setAzDeg = 0;               // ทิศ(°)ที่ลำกล้องชี้อยู่ตอนสั่ง SET_AZ
  // คาลิเบรตเข็มทิศ (Core 0 รับคำสั่ง serial → Core 1 ทำ)
  bool    calRequest = false;         // {"type":"CAL"} เริ่มโหมดหมุน 360°
  bool    calClearReq = false;        // {"type":"CAL_CLEAR"} ล้าง cal กลับค่าดิบ
} state;

// ── Hardware objects ─────────────────────────────────────────────
SPIClass rm_spi(HSPI);
XPowersAXP2101 pmu;
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);
TinyGPSPlus gps;
// ── วินิจฉัย GPS: "ดาวเทียมในสายตา" (GSV) แยก "เสาอากาศ/ฟ้าบัง" ออกจาก "แค่รอเวลา" ──
TinyGPSCustom gpsViewGP(gps, "GPGSV", 3);   // จำนวนดาวเทียม GPS ที่มองเห็น
TinyGPSCustom gpsViewGN(gps, "GNGSV", 3);   // รวมทุกระบบ (u-blox M10)
TinyGPSCustom gpsViewGL(gps, "GLGSV", 3);   // GLONASS
volatile bool gpsRawEcho = false;            // {"type":"GPS_RAW"} = echo NMEA ดิบออก USB (ดู GSV/SNR)
static int gpsSatsInView() {                 // ดาวเทียมในสายตามากสุดจากทุกระบบ (>0 = เสาอากาศรับได้)
  int g = atoi(gpsViewGP.value()), n = atoi(gpsViewGN.value()), l = atoi(gpsViewGL.value());
  int m = g; if (n > m) m = n; if (l > m) m = l;
  return m;
}
HardwareSerial gpsSerial(1);
// จอแนวตั้ง (portrait 64×128): R1 = หมุน 90° CW — ถ้าหัวกลับ (อ่านจากล่างขึ้นบน) สลับเป็น U8G2_R3
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R1, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
#define OLED_X 3   // ระยะร่นจากขอบซ้าย (px) — จอจริงขอบกระจกบังคอลัมน์แรก ข้อความชิดขอบอ่านยาก
Preferences prefs;
// prefs = ออบเจกต์ NVS ตัวเดียว ใช้ทั้งสองคอร์ → ต้องล็อกทุกครั้งที่ begin/end ขณะ task ทำงาน
// (เฉพาะตอนรันคู่กัน; โค้ด setup ก่อนสร้าง task ยังไม่มี mutex → ใช้ prefs ตรง ๆ ได้ ไม่ชน)
static inline void prefsLock()   { if (prefsMutex) xSemaphoreTake(prefsMutex, portMAX_DELAY); }
static inline void prefsUnlock() { if (prefsMutex) xSemaphoreGive(prefsMutex); }

uint8_t gunId = 1;
bool compassOk = false;
// hard/soft-iron cal จากโหมด CAL 360° (โหลดจาก NVS ตอน boot; ยังไม่เคย CAL = ผ่านตรงเหมือนค่าดิบ)
// Core 1 อ่าน/เขียนคนเดียว (setup เขียนก่อนสร้าง task) — ไม่ต้อง mutex
float magOx = 0, magOy = 0;         // ศูนย์กลางวงกลมสนามแม่เหล็ก (hard iron, µT)
float magSx = 1, magSy = 1;         // เมทริกซ์สมมาตร W ดึงวงรีกลับเป็นวงกลม (soft iron): แนวทแยง
float magXY = 0;                    // เทอมไขว้ของ W (วงรีเอียง) — cal เก่า/ยังไม่ CAL = 0 → สูตรเดิมเป๊ะ
bool  magCalHas = false;
bool pmuOk = false;             // AXP2101 (จ่ายไฟ/อ่านแบต) เริ่มต้นสำเร็จไหม
bool bmeOk = false;             // BME280/BMP280 (อากาศ) พร้อมไหม
bool bmeHasHum = false;         // BME280 = มีความชื้น / BMP280 = ไม่มี

volatile bool loraRxFlag = false;
void IRAM_ATTR setRxFlag() { loraRxFlag = true; }

// ── CRC16 ────────────────────────────────────────────────────────
uint16_t crc16(const uint8_t *d, size_t l) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < l; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int j = 0; j < 8; j++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

// ════════════════════════════════════════════════════════════════
// STEP B2 — LoRa link crypto: AES-128-CCM + anti-replay
// (โค้ดชุดเดียวกับ fdc_dongle — envelope/nonce/AAD ต้องตรงกันเป๊ะ)
// Envelope: [0]=0xE1 | [1..4]=counter BE | [5]=sender | cipher | tag 8B
// ════════════════════════════════════════════════════════════════
#define CRYPT_MAGIC    0xE1
#define CRYPT_HDR_LEN  6
#define CRYPT_TAG_LEN  8
#define SENDER_FDC     0xF1   // sender id ของแม่ข่าย

// คีย์เริ่มต้น "FDC-DEFAULT-KEY!" — ทดสอบเท่านั้น ตั้งจริงผ่าน serial:
// {"type":"SET_KEY","key":"<hex 32 ตัว>"} → เก็บ NVS
static uint8_t  loraKey[16] = {
  0x46,0x44,0x43,0x2D,0x44,0x45,0x46,0x41,
  0x55,0x4C,0x54,0x2D,0x4B,0x45,0x59,0x21
};
static uint32_t txCtr = 0;
static uint32_t lastRxCtr[256] = {0};

void cryptInit() {
  prefs.begin("fcs", false);
  prefs.getBytes("lorakey", loraKey, 16);
  txCtr = prefs.getUInt("loractr", 0) + 64;   // boot ใหม่ข้าม +64 กัน nonce ซ้ำ
  prefs.putUInt("loractr", txCtr);
  prefs.end();
}

static void makeNonce(uint8_t sender, uint32_t ctr, uint8_t nonce[13]) {
  memset(nonce, 0, 13);
  nonce[0] = sender;
  nonce[1] = (ctr >> 24) & 0xFF; nonce[2] = (ctr >> 16) & 0xFF;
  nonce[3] = (ctr >>  8) & 0xFF; nonce[4] =  ctr        & 0xFF;
}

size_t cryptSeal(uint8_t sender, const uint8_t *plain, size_t len, uint8_t *out) {
  uint32_t ctr = ++txCtr;
  if ((txCtr & 63) == 0) {
    prefsLock();
    prefs.begin("fcs", false); prefs.putUInt("loractr", txCtr); prefs.end();
    prefsUnlock();
  }
  out[0] = CRYPT_MAGIC;
  out[1] = (ctr >> 24) & 0xFF; out[2] = (ctr >> 16) & 0xFF;
  out[3] = (ctr >>  8) & 0xFF; out[4] =  ctr        & 0xFF;
  out[5] = sender;
  uint8_t nonce[13]; makeNonce(sender, ctr, nonce);
  mbedtls_ccm_context c; mbedtls_ccm_init(&c);
  mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, loraKey, 128);
  int rc = mbedtls_ccm_encrypt_and_tag(&c, len, nonce, 13,
             out, CRYPT_HDR_LEN,
             plain, out + CRYPT_HDR_LEN,
             out + CRYPT_HDR_LEN + len, CRYPT_TAG_LEN);
  mbedtls_ccm_free(&c);
  return rc == 0 ? CRYPT_HDR_LEN + len + CRYPT_TAG_LEN : 0;
}

size_t cryptOpen(const uint8_t *buf, size_t len, uint8_t *out, uint8_t *senderOut) {
  if (len < CRYPT_HDR_LEN + CRYPT_TAG_LEN + 1 || buf[0] != CRYPT_MAGIC) return 0;
  uint32_t ctr = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) |
                 ((uint32_t)buf[3] <<  8) |  (uint32_t)buf[4];
  uint8_t sender = buf[5];
  if (ctr <= lastRxCtr[sender]) return 0;              // replay → ทิ้ง
  size_t plen = len - CRYPT_HDR_LEN - CRYPT_TAG_LEN;
  uint8_t nonce[13]; makeNonce(sender, ctr, nonce);
  mbedtls_ccm_context c; mbedtls_ccm_init(&c);
  mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, loraKey, 128);
  int rc = mbedtls_ccm_auth_decrypt(&c, plen, nonce, 13,
             buf, CRYPT_HDR_LEN,
             buf + CRYPT_HDR_LEN, out,
             buf + CRYPT_HDR_LEN + plen, CRYPT_TAG_LEN);
  mbedtls_ccm_free(&c);
  if (rc != 0) return 0;                               // MAC ไม่ผ่าน = ของปลอม
  lastRxCtr[sender] = ctr;
  if (senderOut) *senderOut = sender;
  return plen;
}

void setLoraKey(const char *hex) {
  if (!hex || strlen(hex) != 32) { Serial.println("[KEY] ERR need 32 hex chars"); return; }
  for (int i = 0; i < 16; i++) {
    char b[3] = { hex[i*2], hex[i*2+1], 0 };
    loraKey[i] = (uint8_t)strtol(b, nullptr, 16);
  }
  prefsLock();
  prefs.begin("fcs", false);
  prefs.putBytes("lorakey", loraKey, 16);
  prefs.end();
  prefsUnlock();
  Serial.println("[KEY] OK saved to NVS");
}

// ── Serial config (USB) — ตั้ง key ตอน provisioning ────────────
String cfgBuf = "";
void handleSerialCfg() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') {
      cfgBuf.trim();
      if (cfgBuf.length() > 0 && cfgBuf[0] == '{') {
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, cfgBuf) == DeserializationError::Ok) {
          const char *t = doc["type"];
          if (t && strcmp(t, "SET_KEY") == 0)
            setLoraKey(doc["key"] | (const char*)nullptr);
          else if (t && strcmp(t, "SET_AZ") == 0) {
            // ตั้งทิศด้วยมือ: ลำกล้องชี้อยู่ที่ "deg" องศา → Core 1 คำนวณ offset ให้
            float az = doc["deg"] | 0.0f;
            if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
              state.setAzDeg = az; state.setAzRequest = true; xSemaphoreGive(xMutex);
            }
            Serial.printf("[LAY] manual SET_AZ %.1f requested\n", (double)az);
          } else if (t && strcmp(t, "GPS_RAW") == 0) {
            gpsRawEcho = !gpsRawEcho;           // สลับ echo NMEA ดิบออก USB (ดู GSV/SNR ตรง ๆ)
            Serial.printf("[GPS] raw echo %s\n", gpsRawEcho ? "ON" : "OFF");
          } else if (t && strcmp(t, "CAL") == 0) {
            // เริ่มโหมดคาลิเบรตเข็มทิศ (เท่ากับกด BOOT ค้าง ≥6s) — Core 1 เป็นคนทำ
            if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
              state.calRequest = true; xSemaphoreGive(xMutex);
            }
            Serial.println("[CAL] requested via serial");
          } else if (t && strcmp(t, "CAL_CLEAR") == 0) {
            if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
              state.calClearReq = true; xSemaphoreGive(xMutex);
            }
          }
        }
      }
      cfgBuf = "";
    } else {
      cfgBuf += ch;
      if (cfgBuf.length() > 256) cfgBuf = "";
    }
  }
}

// ── RM3100 SPI Helpers ───────────────────────────────────────────
uint8_t rm_read_reg(uint8_t reg) {
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(reg | 0x80);
  uint8_t val = rm_spi.transfer(0x00);
  digitalWrite(RM_CS, HIGH);
  return val;
}

void rm_write_reg(uint8_t reg, uint8_t val) {
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(reg & 0x7F);
  rm_spi.transfer(val);
  digitalWrite(RM_CS, HIGH);
}

void rm_write_cc(uint8_t reg, uint16_t cc) {
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(reg & 0x7F);
  rm_spi.transfer((cc >> 8) & 0xFF);
  rm_spi.transfer(cc & 0xFF);
  digitalWrite(RM_CS, HIGH);
}

static int32_t to_signed24(uint8_t a, uint8_t b, uint8_t c) {
  int32_t v = ((int32_t)a << 16) | ((int32_t)b << 8) | c;
  if (v & 0x800000) v |= 0xFF000000;
  return v;
}

bool rm3100_init() {
  pinMode(RM_CS, OUTPUT);
  digitalWrite(RM_CS, HIGH);
  rm_spi.begin(RM_CLK, RM_MISO, RM_MOSI, RM_CS);
  rm_spi.setFrequency(1000000);
  rm_spi.setDataMode(SPI_MODE0);
  delay(10);
  uint8_t id = rm_read_reg(RM3100_REVID);
  Serial.printf("[RM3100] REVID=0x%02X (expect 0x22)\n", id);
  if (id != 0x22) return false;
  rm_write_cc(RM3100_REG_CCX, RM3100_CYCLE);
  rm_write_cc(RM3100_REG_CCY, RM3100_CYCLE);
  rm_write_cc(RM3100_REG_CCZ, RM3100_CYCLE);
  rm_write_reg(RM3100_REG_CMM, 0x71);
  delay(10);
  return true;
}

bool rm3100_read(RM3100Data &out) {
  if (!(rm_read_reg(RM3100_REG_STATUS) & 0x80)) return false;
  uint8_t buf[9];
  digitalWrite(RM_CS, LOW);
  rm_spi.transfer(RM3100_REG_MX | 0x80);
  for (int i = 0; i < 9; i++) buf[i] = rm_spi.transfer(0x00);
  digitalWrite(RM_CS, HIGH);
  out.x = to_signed24(buf[0], buf[1], buf[2]) * RM3100_SCALE;
  out.y = to_signed24(buf[3], buf[4], buf[5]) * RM3100_SCALE;
  out.z = to_signed24(buf[6], buf[7], buf[8]) * RM3100_SCALE;
  return true;
}

// ── BME280/BMP280 (อุณหภูมิ/ความกดอากาศ/ความชื้น) — บัส Wire (17/18) ร่วมกับ OLED ───
// สำหรับชดเชยความหนาแน่นอากาศในการคำนวณยิง (ตรง "การขยายผล" ในฟอร์ม)
struct BmeCalib {
  uint16_t T1; int16_t T2, T3;
  uint16_t P1; int16_t P2,P3,P4,P5,P6,P7,P8,P9;
  uint8_t H1, H3; int16_t H2, H4, H5; int8_t H6;
};
BmeCalib bmeCal;
uint8_t bmeAddr = 0x76;

static uint8_t bme_r8(uint8_t reg) {
  Wire.beginTransmission(bmeAddr); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom((int)bmeAddr, 1);
  return Wire.available() ? Wire.read() : 0;
}
static void bme_w8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(bmeAddr); Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint16_t bme_r16LE(uint8_t reg) { return bme_r8(reg) | (bme_r8(reg+1) << 8); }

bool bme_init() {
  uint8_t addrs[2] = {0x76, 0x77};
  for (int i = 0; i < 2; i++) {
    bmeAddr = addrs[i];
    uint8_t id = bme_r8(0xD0);              // chip id: 0x60=BME280, 0x58=BMP280
    if (id == 0x60 || id == 0x58) {
      bmeHasHum = (id == 0x60);
      bmeCal.T1 = bme_r16LE(0x88); bmeCal.T2 = (int16_t)bme_r16LE(0x8A);
      bmeCal.T3 = (int16_t)bme_r16LE(0x8C);
      bmeCal.P1 = bme_r16LE(0x8E); bmeCal.P2 = (int16_t)bme_r16LE(0x90);
      bmeCal.P3 = (int16_t)bme_r16LE(0x92); bmeCal.P4 = (int16_t)bme_r16LE(0x94);
      bmeCal.P5 = (int16_t)bme_r16LE(0x96); bmeCal.P6 = (int16_t)bme_r16LE(0x98);
      bmeCal.P7 = (int16_t)bme_r16LE(0x9A); bmeCal.P8 = (int16_t)bme_r16LE(0x9C);
      bmeCal.P9 = (int16_t)bme_r16LE(0x9E);
      if (bmeHasHum) {
        bmeCal.H1 = bme_r8(0xA1); bmeCal.H2 = (int16_t)bme_r16LE(0xE1);
        bmeCal.H3 = bme_r8(0xE3);
        uint8_t e4 = bme_r8(0xE4), e5 = bme_r8(0xE5), e6 = bme_r8(0xE6);
        bmeCal.H4 = (int16_t)(((int8_t)e4 << 4) | (e5 & 0x0F));   // signed 12-bit
        bmeCal.H5 = (int16_t)(((int8_t)e6 << 4) | (e5 >> 4));
        bmeCal.H6 = (int8_t)bme_r8(0xE7);
        bme_w8(0xF2, 0x01);                  // ctrl_hum: humidity oversampling x1
      }
      bme_w8(0xF5, 0xA0);                     // config: t_standby 1000ms, filter off
      bme_w8(0xF4, 0x27);                     // ctrl_meas: temp x1, press x1, normal
      delay(50);
      return true;
    }
  }
  return false;
}

// อ่าน met (°C, Pa, %RH) — บัส Wire (Core 1 คนเดียว ร่วม OLED) ไม่ต้อง mutex (สูตร Bosch ทดสอบ gcc แล้ว)
bool bme_read(float &T, float &P, float &H) {
  Wire.beginTransmission(bmeAddr); Wire.write(0xF7); Wire.endTransmission(false);
  Wire.requestFrom((int)bmeAddr, 8);
  if (Wire.available() < 8) return false;
  uint8_t d[8]; for (int i=0;i<8;i++) d[i]=Wire.read();
  int32_t adcP = ((int32_t)d[0]<<12)|((int32_t)d[1]<<4)|(d[2]>>4);
  int32_t adcT = ((int32_t)d[3]<<12)|((int32_t)d[4]<<4)|(d[5]>>4);
  int32_t adcH = ((int32_t)d[6]<<8)|d[7];

  float v1 = ((float)adcT/16384.0f - (float)bmeCal.T1/1024.0f) * (float)bmeCal.T2;
  float v2 = ((float)adcT/131072.0f - (float)bmeCal.T1/8192.0f);
  v2 = v2*v2*(float)bmeCal.T3;
  float tfine = v1 + v2;
  T = tfine/5120.0f;

  v1 = tfine/2.0f - 64000.0f;
  v2 = v1*v1*(float)bmeCal.P6/32768.0f;
  v2 = v2 + v1*(float)bmeCal.P5*2.0f;
  v2 = v2/4.0f + (float)bmeCal.P4*65536.0f;
  v1 = ((float)bmeCal.P3*v1*v1/524288.0f + (float)bmeCal.P2*v1)/524288.0f;
  v1 = (1.0f + v1/32768.0f)*(float)bmeCal.P1;
  if (v1 == 0.0f) { P = 0; }
  else {
    float p = 1048576.0f - (float)adcP;
    p = (p - v2/4096.0f)*6250.0f/v1;
    v1 = (float)bmeCal.P9*p*p/2147483648.0f;
    v2 = p*(float)bmeCal.P8/32768.0f;
    P = p + (v1+v2+(float)bmeCal.P7)/16.0f;
  }

  if (bmeHasHum) {
    float h = tfine - 76800.0f;
    h = ((float)adcH - ((float)bmeCal.H4*64.0f + (float)bmeCal.H5/16384.0f*h))
        * ((float)bmeCal.H2/65536.0f*(1.0f + (float)bmeCal.H6/67108864.0f*h
           *(1.0f+(float)bmeCal.H3/67108864.0f*h)));
    h = h*(1.0f - (float)bmeCal.H1*h/524288.0f);
    if (h>100.0f) h=100.0f; if (h<0.0f) h=0.0f;
    H = h;
  } else H = 0;
  return true;
}

// ── I2C scanner — พิมพ์ address ที่เจอ (ใช้ยืนยันว่าบอร์ดมีอะไรจริง) ──
void i2cScan(TwoWire &bus, const char *name) {
  int n = 0;
  Serial.printf("[I2C %s] scan:", name);
  for (uint8_t a = 1; a < 127; a++) {
    bus.beginTransmission(a);
    if (bus.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.printf("  (%d found)\n", n);
}

// ── ทิศ (องศา) จาก RM3100 ผ่าน hard/soft-iron cal (one-shot จากโหมด CAL 360°) → true north ──
// ไม่มี cal ต่อเนื่อง (ตัดทิ้ง — มันทำทิศค้างตอนหัน); ยังไม่เคย CAL → offset 0/scale 1 = ดิบเดิม
// hard-iron ส่วนที่เหลือ + declination ยังถูกดูดใน layOffset ตอน "ตั้งทิศเหนือ" เหมือนเดิม
static float headingFromMag(const RM3100Data &m) {
  float ux = m.x - magOx, uy = m.y - magOy;
  float cx = ux * magSx + uy * magXY;   // W สมมาตร [Sx,XY;XY,Sy] — XY=0 = สเกลแกนตรงแบบเดิม
  float cy = ux * magXY + uy * magSy;
  float d = atan2f(cx, -cy) * 180.0f / PI;
  if (d < 0) d += 360.0f;
#if HDG_REVERSE
  d = 360.0f - d;                             // กลับทิศหมุนให้ตรงกับ iPhone
#endif
  d += MAG_DECLINATION + HDG_TRIM_DEG;        // → true north + จูนติดตั้งครั้งเดียว
  while (d < 0)       d += 360.0f;
  while (d >= 360.0f) d -= 360.0f;
  return d;
}

// ── แก้ระบบสมการ 3 ตัวแปร [A|b] (Gauss-Jordan + partial pivot) — คืน false ถ้า singular ──
static bool gauss3(double A[3][4]) {
  for (int col = 0; col < 3; col++) {
    int piv = col;
    for (int r = col+1; r < 3; r++) if (fabs(A[r][col]) > fabs(A[piv][col])) piv = r;
    if (fabs(A[piv][col]) < 1e-9) return false;
    if (piv != col) for (int k = 0; k < 4; k++) { double t = A[col][k]; A[col][k] = A[piv][k]; A[piv][k] = t; }
    for (int r = 0; r < 3; r++) if (r != col) {
      double f = A[r][col] / A[col][col];
      for (int k = col; k < 4; k++) A[r][k] -= f * A[col][k];
    }
  }
  return true;
}

// ── fit ผลหมุน 360°: วงกลม least-squares (Kåsa) หา hard-iron + วงรีเต็ม "มีเทอมไขว้"
//    หา soft-iron (v7.4 — รับวงรีเอียงที่แกนไม่ตรง X/Y ซึ่งสเกลแกนตรงเดิมแก้ไม่ได้) ──
// ผลลัพธ์ = เมทริกซ์สมมาตร W=[sxx,sxy;sxy,syy] ดึงวงรีกลับเป็นวงกลมรัศมี rm:
//   fit A·u²+B·v²+2C·uv=1 → Q=[A,C;C,B] → W = rm·√Q (√Q ปิดรูป 2×2: (Q+√det·I)/√(tr+2√det))
// ฟิตเต็มเสื่อม (จุดพอดีเส้น/ได้ไฮเพอร์โบลา) → ถอยไปวงรีแกนตรงแบบเดิมอัตโนมัติ (sxy=0)
// คืน false ถ้าข้อมูลเสื่อม (จุดน้อย/กระจุก/วงรีเบี้ยวเกิน 2:1 = เหล็กแรงมากหรือหมุนไม่ครบ)
static bool magCalFit(const float pts[][2], uint16_t n, float &ox, float &oy,
                      float &sxx, float &syy, float &sxy) {
  if (n < CAL_MIN_PTS) return false;
  // 1) Kåsa: x²+y² = 2a·x + 2b·y + c → แก้ normal equations 3×3 → ศูนย์กลาง (hard iron)
  double Sx=0,Sy=0,Sxx=0,Syy=0,Sxy=0,Sxz=0,Syz=0,Sz=0;
  for (uint16_t i = 0; i < n; i++) {
    double x = pts[i][0], y = pts[i][1], z = x*x + y*y;
    Sx+=x; Sy+=y; Sxx+=x*x; Syy+=y*y; Sxy+=x*y; Sxz+=x*z; Syz+=y*z; Sz+=z;
  }
  double K[3][4] = {{Sxx,Sxy,Sx,Sxz},{Sxy,Syy,Sy,Syz},{Sx,Sy,(double)n,Sz}};
  if (!gauss3(K)) return false;
  double a = K[0][3]/K[0][0]/2.0, b = K[1][3]/K[1][1]/2.0, c = K[2][3]/K[2][2];
  if (c + a*a + b*b <= 0) return false;               // รัศมี² ต้องเป็นบวก
  // 2) วงรีเต็มรอบศูนย์จากข้อ 1: A·u² + B·v² + 2C·uv = 1 (least squares 3 ตัวแปร)
  double Su4=0,Sv4=0,Su2v2=0,Su3v=0,Suv3=0,Su2=0,Sv2=0,Suv=0;
  for (uint16_t i = 0; i < n; i++) {
    double u = pts[i][0]-a, v = pts[i][1]-b, u2 = u*u, v2 = v*v;
    Su4+=u2*u2; Sv4+=v2*v2; Su2v2+=u2*v2; Su3v+=u2*u*v; Suv3+=u*v2*v;
    Su2+=u2; Sv2+=v2; Suv+=u*v;
  }
  double eA=0, eB=0, eC=0;
  double E[3][4] = {{Su4,Su2v2,2.0*Su3v,Su2},{Su2v2,Sv4,2.0*Suv3,Sv2},{Su3v,Suv3,2.0*Su2v2,Suv}};
  bool full = gauss3(E);
  if (full) {
    eA = E[0][3]/E[0][0]; eB = E[1][3]/E[1][1]; eC = E[2][3]/E[2][2];
    full = eA > 0 && eB > 0 && (eA*eB - eC*eC) > 0;   // ต้องเป็นวงรีจริง ไม่ใช่ไฮเพอร์โบลา
  }
  if (!full) {                                        // fallback: วงรีแกนตรงแบบเดิม (v7.2)
    double det = Su4*Sv4 - Su2v2*Su2v2;
    if (fabs(det) < 1e-12) return false;
    eA = (Su2*Sv4 - Sv2*Su2v2)/det; eB = (Su4*Sv2 - Su2v2*Su2)/det; eC = 0;
    if (eA <= 0 || eB <= 0) return false;
  }
  // ครึ่งแกนจริงจาก eigenvalue ของ Q — guard ความเบี้ยววัดตามแกนวงรีจริง (รู้ทันวงรีเอียง:
  // เดิมวงรี 2:1 เอียง 45° ฟิตแกนตรงมองเป็น "วงกลม" → ผ่าน guard แต่ไม่ได้แก้อะไรเลย)
  double mid = (eA + eB)/2.0, dif = sqrt((eA-eB)*(eA-eB)/4.0 + eC*eC);
  double l1 = mid + dif, l2 = mid - dif;              // λmax, λmin ของ Q
  if (l2 <= 1e-12) return false;
  double rmax = 1.0/sqrt(l2), rmin = 1.0/sqrt(l1);    // ครึ่งแกนยาว/สั้น
  if (rmax/rmin > 2.0) return false;                  // เบี้ยวเกิน = ข้อมูลเสีย/เหล็กแรงมาก ไม่รับ
  double rm = (rmax + rmin)/2.0;
  double sd = sqrt(eA*eB - eC*eC), tau = sqrt(eA + eB + 2.0*sd);
  ox = (float)a; oy = (float)b;
  sxx = (float)(rm*(eA + sd)/tau);                    // W = rm·√Q — คูณแล้ววงรีกลับเป็นวงกลมรัศมี rm
  syy = (float)(rm*(eB + sd)/tau);
  sxy = (float)(rm*eC/tau);
  return true;
}

// ── พิกัด lat/lon (WGS84) → MGRS "ZB SQ EEEE NNNN" (8 หลัก, ละเอียด 10 m) ──
// ทหารปืนใหญ่ใช้ "พิกัดกริด" ไม่ใช่องศาทศนิยม — แปลงผ่าน UTM (Transverse Mercator)
// (สูตร Snyder มาตรฐาน ทดสอบเทียบจุดอ้างอิงแล้ว: easting บน CM = 500000.00 เป๊ะ)
void latlon_to_mgrs(double lat, double lon, char *out, int n) {
  double a=6378137.0, f=1.0/298.257223563, k0=0.9996;
  double e2=f*(2-f), ep2=e2/(1-e2);
  double latR=lat*PI/180.0, lonR=lon*PI/180.0;
  int zone=(int)floor(lon/6.0)+31;
  double lon0=((zone-1)*6 - 180 + 3)*PI/180.0;
  double Nn=a/sqrt(1-e2*sin(latR)*sin(latR));
  double T=tan(latR)*tan(latR), C=ep2*cos(latR)*cos(latR);
  double A=cos(latR)*(lonR-lon0);
  double M=a*((1-e2/4-3*e2*e2/64-5*e2*e2*e2/256)*latR
            -(3*e2/8+3*e2*e2/32+45*e2*e2*e2/1024)*sin(2*latR)
            +(15*e2*e2/256+45*e2*e2*e2/1024)*sin(4*latR)
            -(35*e2*e2*e2/3072)*sin(6*latR));
  double easting=k0*Nn*(A+(1-T+C)*A*A*A/6
            +(5-18*T+T*T+72*C-58*ep2)*A*A*A*A*A/120)+500000.0;
  double northing=k0*(M+Nn*tan(latR)*(A*A/2
            +(5-T+9*C+4*C*C)*A*A*A*A/24
            +(61-58*T+T*T+600*C-330*ep2)*A*A*A*A*A*A/720));
  if(lat<0) northing+=10000000.0;
  const char* bands="CDEFGHJKLMNPQRSTUVWX";
  int bi=(int)floor((lat+80.0)/8.0); if(bi<0)bi=0; if(bi>19)bi=19;
  static const char* col[3]={"ABCDEFGH","JKLMNPQR","STUVWXYZ"};
  const char* rowL="ABCDEFGHJKLMNPQRSTUV";
  int set=(zone-1)%3;
  int ci=(int)(easting/100000.0)-1; if(ci<0)ci=0; if(ci>7)ci=7;
  int ri=((int)(northing/100000.0)%20 + ((zone%2==0)?5:0))%20;
  int e4=(int)(fmod(easting,100000.0)/10.0);
  int n4=(int)(fmod(northing,100000.0)/10.0);
  snprintf(out, n, "%d%c %c%c %04d %04d", zone, bands[bi], col[set][ci], rowL[ri], e4, n4);
}

// ── LoRa transmit (Core 0) — v7.2: ไม่ทิ้ง GPS ระหว่างส่ง ─────────
// STEP B2: เข้ารหัสทุกแพ็กเก็ตก่อนออกอากาศ (sender = gunId)
// เดิม radio.transmit() บล็อกทั้ง airtime (SF12 = 3.6s → GPS ล้น + หูหนวก) —
// ตอนนี้ startTransmit แล้ววนอ่าน GPS จน DIO1 แจ้ง TX-done (ต้องใช้ RadioLib ≥6.0)
void loraSend(uint8_t *data, size_t len) {
  uint8_t env[80];
  size_t n = cryptSeal(gunId, data, len, env);
  if (n) {
    loraRxFlag = false;                              // จากนี้จนจบ DIO1 = TX-done
    if (radio.startTransmit(env, n) == RADIOLIB_ERR_NONE) {
      uint32_t t0 = millis();
      while (!loraRxFlag && millis() - t0 < 1500) {  // airtime จริง ~0.25s ที่ SF9 (เผื่อถึง 1.5s)
        while (gpsSerial.available()) {
          char c = gpsSerial.read();
          gps.encode(c);
          if (gpsRawEcho) Serial.write(c);
        }
        vTaskDelay(1);
      }
      loraRxFlag = false;
      radio.finishTransmit();                        // เคลียร์ IRQ + กลับ standby
    }
  }
  radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════
// CORE 0 TASK — GPS + LoRa
// ═══════════════════════════════════════════════════════════════
void taskRadioGPS(void *pv) {
  uint32_t lastStatus = 0;
  uint32_t statusInterval = 5000;
  uint32_t lastGpsDbg = 0;
  uint32_t lastBattMs = 0;        // อ่านแบตทุก 5s (ช้า ๆ ลดการชน Wire1)
  uint8_t  battVal = 255;         // % แบตที่จะรายงาน (255 = ไม่มีแบต/USB) — เก็บค่าเดิมถ้าอ่านพลาด
  uint8_t  battSmooth = 0;        // ค่า smooth (EMA) กันเด้งตอน LoRa TX โหลดหนัก
  bool     battSeen = false;

  for (;;) {
    // GPS
    while (gpsSerial.available()) {
      char c = gpsSerial.read();
      gps.encode(c);
      if (gpsRawEcho) Serial.write(c);    // echo ดิบ → แปะให้ดู GSV/SNR ได้ตรง ๆ
    }

    // วินิจฉัย GPS ทุก 3 วิ — แยกซอฟต์แวร์/ฮาร์ดแวร์:
    //   chars=0            → ไม่มีข้อมูลเข้าเลย = สาย/ไฟ/พิน/baud/โมดูลเสีย (HW)
    //   chars>0 sats=0/fix0 → โมดูลพูดแล้วแต่ยังไม่จับฟ้า = cold start/เสาอากาศ/อยู่ในร่ม
    //   csumErr พุ่ง        → baud ไม่ตรง/สัญญาณรบกวน
    if (millis()-lastGpsDbg >= 3000) {
      lastGpsDbg = millis();
      int inView = gpsSatsInView();
      Serial.printf("[GPS] chars=%lu view=%d used=%d fix=%d hdop=%.1f csumErr=%lu  %s\n",
                    (unsigned long)gps.charsProcessed(), inView, gps.satellites.value(),
                    (int)gps.location.isValid(), gps.hdop.hdop(),
                    (unsigned long)gps.failedChecksum(),
                    gps.charsProcessed() < 50 ? "NO-DATA wiring/baud"
                    : inView == 0 ? "0-in-view ANTENNA/indoor"
                    : !gps.location.isValid() ? "acquiring need-sky+time" : "FIX-OK");
    }

    // ── อ่านแบตทุก 5s: เก็บค่าเดิมถ้าอ่านพลาด + smooth กันเด้งตอน LoRa TX ดึงไฟ ──
    // (เดิมอ่านทุกรอบ พอ mutex ชน battNow=0 → state.battPct เด้งเป็น 0 = "ขึ้นมั่ว")
    if (!battSeen || millis() - lastBattMs >= 5000) {
      if (xSemaphoreTake(wire1Mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        uint16_t vbat = pmu.getBattVoltage();             // mV
        xSemaphoreGive(wire1Mutex);
        if (vbat > 2600 && vbat < 4500) {                 // มีแบตจริง → คิด %
          uint8_t p = (uint8_t)constrain(map(vbat, 3300, 4200, 0, 100), 0, 100);
          battSmooth = battSeen ? (uint8_t)((battSmooth * 3 + p) / 4) : p;
          battVal = battSmooth;
        } else {
          battVal = 255;                                  // ไม่มีแบต/เสียบ USB เปล่า → จอโชว์ "USB"
        }
        battSeen = true; lastBattMs = millis();
      }
      // อ่าน Wire1 ไม่ได้ (mutex ไม่ว่าง) → ไม่อัปเดต → เก็บ battVal เดิม ไม่เด้งเป็น 0
    }
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      if (gps.location.isValid()) {
        state.lat = gps.location.lat();
        state.lon = gps.location.lng();
        state.alt = gps.altitude.meters();
        state.gpsValid = true;
      }
      // COG + ความเร็ว (true-north course จาก RMC) → Core 1 ตั้งทิศเหนืออัตโนมัติ
      if (gps.course.isValid() && gps.speed.isValid() && gps.course.age() < 2000) {
        state.cog      = gps.course.deg();
        state.speedKmh = gps.speed.kmph();
        state.cogValid = true;
      } else {
        state.cogValid = false;
      }
      state.sats = gps.satellites.value();
      state.satsView = (uint8_t)gpsSatsInView();
      state.battPct = battVal;
      xSemaphoreGive(xMutex);
    }

    // Serial config (provisioning key ผ่าน USB)
    handleSerialCfg();

    // LoRa RX
    if (loraRxFlag) {
      loraRxFlag = false;
      uint8_t env[80], buf[80];   // buf ≥ env — กันถอดรหัส (plen) ล้น stack
      int st = radio.readData(env, sizeof(env));
      if (st == RADIOLIB_ERR_NONE) {
        size_t envLen = radio.getPacketLength();
        if (envLen > sizeof(env)) envLen = sizeof(env);   // clamp: getPacketLength ได้ถึง 255 → กันถอดเกินที่รับมาจริง (buf overflow)
        // STEP B2: ถอดรหัส + MAC + กัน replay — รับเฉพาะแม่ข่าย (SENDER_FDC)
        uint8_t sender = 0;
        size_t len = cryptOpen(env, envLen, buf, &sender);
        if (len >= 4 && sender == SENDER_FDC) {
          uint16_t rxCrc = ((uint16_t)buf[len-2]<<8)|buf[len-1];
          if (rxCrc == crc16(buf, len-2)) {
            // ได้ยินแม่ข่ายจริง (ผ่าน MAC+CRC) → อัปเดตสถานะลิงก์ + RSSI
            if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              state.lastFdcRx = millis();
              state.lastRssi  = (int16_t)radio.getRSSI();
              xSemaphoreGive(xMutex);
            }
            if (buf[0]==0x10 && buf[2]==gunId && len>=11) {
              if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                state.azMils  = ((int16_t)buf[4]<<8)|buf[5];
                state.elMils  = ((int16_t)buf[6]<<8)|buf[7];
                state.charge  = buf[8];
                state.aimSeq  = buf[3];
                state.hasCmd  = true;
                state.cmdTime = millis();
                xSemaphoreGive(xMutex);
              }
              Serial.printf("[GUN%d] FIRE CMD az=%d el=%d chg=%d\n",
                            gunId, ((int16_t)buf[4]<<8)|buf[5],
                            ((int16_t)buf[6]<<8)|buf[7], buf[8]);
              uint8_t ack[6];
              ack[0]=0x11; ack[1]=gunId; ack[2]=buf[3]; ack[3]=0x01;
              uint16_t ac = crc16(ack,4);
              ack[4]=(ac>>8)&0xFF; ack[5]=ac&0xFF;
              loraSend(ack, 6);
            } else if (buf[0]==0x01 && buf[2]==gunId) {
              uint8_t pong[6];
              pong[0]=0x02; pong[1]=gunId; pong[2]=0; pong[3]=0;
              uint16_t pc = crc16(pong,4);
              pong[4]=(pc>>8)&0xFF; pong[5]=pc&0xFF;
              loraSend(pong, 6);
            }
          }
        }
      }
      radio.startReceive();
    }

    // Fire command timeout
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      if (state.hasCmd && millis()-state.cmdTime > 30000) state.hasCmd = false;
      xSemaphoreGive(xMutex);
    }

    // STATUS broadcast
    if (millis()-lastStatus >= statusInterval) {
      lastStatus = millis();
      statusInterval = 5000 + random(1000);

      uint8_t p[20];
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        p[0]=0x20; p[1]=gunId; p[2]=0;
        float lat=(float)state.lat, lon=(float)state.lon;
        memcpy(&p[3],&lat,4); memcpy(&p[7],&lon,4);
        p[11]=(state.azMils>>8)&0xFF; p[12]=state.azMils&0xFF;
        p[13]=(state.elMils>>8)&0xFF; p[14]=state.elMils&0xFF;
        p[15]=state.battPct;
        uint16_t hdgInt = state.hdgValid ? (uint16_t)state.hdgMils : 0xFFFF;
        p[16]=(hdgInt>>8)&0xFF; p[17]=hdgInt&0xFF;
        xSemaphoreGive(xMutex);
      }
      uint16_t c = crc16(p,18); p[18]=(c>>8)&0xFF; p[19]=c&0xFF;
      loraSend(p, 20);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ═══════════════════════════════════════════════════════════════
// CORE 1 TASK — Compass + Display + Button
// ═══════════════════════════════════════════════════════════════
void taskCompassDisplay(void *pv) {
  uint32_t lastDisplay = 0;
  uint32_t lastDebug = 0;
  uint32_t btnPressTime = 0;
  bool btnPrev = false;
  int  btnStage = 0;            // 0=none 1=will SET NORTH (กดค้าง ≥2s)

  // ทิศจาก RM3100 (atan2 ผ่าน one-shot cal + EMA) — Core 1 ถือคนเดียว
  float fusedHdg = 0; bool fuseInit = false;   // fusedHdg = ทิศเข็มทิศปัจจุบัน (ชื่อเดิมจากตอนมี fusion)
  float smS = 0, smC = 0; bool smInit = false; // เวกเตอร์ EMA ของทิศ (HDG_SMOOTH_A) — wrap-safe
  bool  hdgFresh = false;                      // v7.4: fusedHdg คำนวณจากเฟรมจริงหลังรีอาร์มล่าสุดแล้ว (กันใช้ทิศค้าง)
  uint32_t lastMagOk = millis();     // อ่าน RM3100 สำเร็จล่าสุด (เฟรมจริง) — เกิน COMPASS_LOST_MS = หลุด
  uint32_t lastReinit = 0;      // millis() ลองรีอาร์ม RM3100 ล่าสุด (กันรีอาร์มถี่เกินตอนเซนเซอร์หลุด)
  uint32_t lastMagChange = millis(); // ค่าสนามแม่เหล็กขยับล่าสุด — ไม่ขยับนาน = CMM ค้าง (cold boot)
  float    prevMagX = 1e9f, prevMagY = 1e9f;  // ค่ารอบก่อน (sentinel 1e9 = เฟรมถัดไปเป็น baseline ไม่นับเป็นทิศ)
  bool     hdgWasValid = false; // เคยอ่านทิศได้แล้วหรือยัง (แยก "กำลังเริ่ม" ออกจาก "หลุด")
  uint32_t lastMet = 0;        // อ่าน BME280 (อากาศ) ทุก 2 วิ

  // ── โหมดคาลิเบรต hard/soft-iron: หมุนปืน 360° ครั้งเดียวจบ (BOOT ≥6s / {"type":"CAL"}) ──
  static float calPts[CAL_MAX_PTS][2];   // จุดดิบ X/Y ระหว่างหมุน (static = อยู่นอก stack ของ task)
  bool     calActive = false;
  uint32_t calStart = 0, lastCalBin = 0, calMsgUntil = 0;
  uint16_t calN = 0;
  uint8_t  calSkip = 0, calBinCnt = 0, calMsg = 0;   // calMsg: 1=OK 2=FAIL 3=CANCEL
  bool     calBins[CAL_BINS];
  uint8_t  layMsg = 0; uint32_t layMsgUntil = 0;     // v7.4: จอแจ้ง "SET N FAIL" (ตั้งเหนือตอนเข็มทิศไม่พร้อม)

  // ── auto-lay (ตั้งทิศเหนือจาก GPS course-over-ground) — Core 1 ถือคนเดียว ──
  float    layOffset = 0;       // ทิศจริง = fusedHdg + layOffset
  bool     layValid  = false;   // ได้อ้างอิงเหนือจริงแล้วหรือยัง
  bool     layFresh  = false;   // ตั้งในรอบทำงานนี้ (true) หรือโหลดจาก NVS เซสชันก่อน (false→โชว์ "?")
  uint8_t  laySource = 0;       // 0=ยังไม่ได้ตั้ง 1=GPS-track (เผื่อ Tier 1 ในอนาคต)
  float    savedLayOff = 0; uint32_t lastLaySave = 0;
  bool     savedLayHas = false; // v7.4: NVS มี layHas=1 แล้วหรือยัง — ล็อกแรกเซฟเสมอแม้ offset≈0 (เดิมหายตอนรีบูต)
  float    laySinC=0, layCosC=0, laySinM=0, layCosM=0;   // ตัวสะสม circular-mean ของหน้าต่าง
  uint16_t laySamp=0; uint32_t layWinStart=0;
  float    laySpdSum=0;         // v7.3: ผลรวมความเร็วในหน้าต่าง → เฉลี่ย = น้ำหนักหน้าต่าง
  float    layWeight=0;         // v7.3: น้ำหนักหลักฐานสะสมของ layOffset (เยอะ = เชื่อค่าเดิมมาก)
  float    layPendOff=0, layPendW=0; bool layPendValid=false;  // v7.3: หน้าต่างแรกที่รอคอนเฟิร์ม
  // โหลด lay เดิม — ถ้าเคยตั้งและยังไม่ขยับ ใช้ได้ทันทีหลังรีบูต (จอโชว์ "N-SET?" เตือนว่าค้างจากเซสชันก่อน)
  prefsLock();
  prefs.begin("fcs", true);
  layOffset = prefs.getFloat("layOff", 0.0f);
  layValid  = prefs.getUChar("layHas", 0) ? true : false;
  prefs.end();
  prefsUnlock();
  laySource = layValid ? 1 : 0; savedLayOff = layOffset; savedLayHas = layValid;
  if (layValid) layWeight = 50.0f;   // v7.3: ค่าเก่าจาก NVS = เชื่อระดับกลาง (หลักฐานใหม่ยังดึงได้ไว)

  // ตั้งทิศด้วยมือ (ปุ่ม 2s / serial SET_AZ): ลำกล้องชี้ desiredDeg อยู่ตอนนี้ → คำนวณ offset
  // ให้ทิศที่แสดงอ่านได้ = desiredDeg.  เก็บ NVS เหมือน auto-lay (laySource=2=manual)
  auto applyManualLay = [&](float desiredDeg) {
    layOffset = desiredDeg - fusedHdg;
    while (layOffset < 0.0f)    layOffset += 360.0f;
    while (layOffset >= 360.0f) layOffset -= 360.0f;
    layValid = true; layFresh = true; laySource = 2; layMsg = 0;
    layWeight = 100.0f; layPendValid = false;   // v7.3: ตั้งมือ = เชื่อสูง + ล้างหน้าต่างรอคอนเฟิร์ม
    prefsLock();
    prefs.begin("fcs", false);
    prefs.putFloat("layOff", layOffset); prefs.putUChar("layHas", 1);
    prefs.end();
    prefsUnlock();
    savedLayOff = layOffset; savedLayHas = true; lastLaySave = millis();
    Serial.printf("[LAY] manual set %.1f -> off=%.1f (mag=%.1f)\n",
                  (double)desiredDeg, (double)layOffset, (double)fusedHdg);
  };

  // ── เริ่ม/จบโหมด CAL 360° — fit วงกลม/วงรีจากจุดที่เก็บ แล้วเซฟ NVS ──
  auto calBegin = [&]() {
    calActive = true; calStart = millis(); lastCalBin = 0;
    calN = 0; calSkip = 0; calBinCnt = 0; calMsg = 0;
    memset(calBins, 0, sizeof(calBins));
    Serial.println("[CAL] start — rotate gun one full slow turn (~60s)");
  };
  auto calFinish = [&]() {
    calActive = false;
    float ox, oy, sx, sy, sxy;
    bool ok = magCalFit(calPts, calN, ox, oy, sx, sy, sxy);
    if (ok) {          // เช็คซ้ำว่าครอบคลุม ≥330° รอบศูนย์กลางที่ fit ได้จริง (ไม่ใช่ค่าประมาณ)
      bool bins[CAL_BINS]; memset(bins, 0, sizeof(bins)); int bc = 0;
      for (uint16_t i = 0; i < calN; i++) {
        int bi = (int)((atan2f(calPts[i][1] - oy, calPts[i][0] - ox) + PI) / (2.0f * PI) * CAL_BINS);
        if (bi < 0) bi = 0; if (bi >= CAL_BINS) bi = CAL_BINS - 1;
        if (!bins[bi]) { bins[bi] = true; bc++; }
      }
      ok = bc >= CAL_MIN_BINS;
    }
    if (ok) {
      magOx = ox; magOy = oy; magSx = sx; magSy = sy; magXY = sxy; magCalHas = true;
      // cal เปลี่ยนนิยามทิศ → layOffset เดิมผิดความหมาย: ล้างทิ้ง บังคับตั้งเหนือใหม่ (จอเตือน)
      layValid = false; layFresh = false; laySource = 0; layOffset = 0;
      savedLayOff = 0; savedLayHas = false;
      layWeight = 0; layPendValid = false;   // v7.3: ล้างหลักฐานสะสม เริ่มนับใหม่หลัง cal
      smS = smC = 0; smInit = false;
      prefsLock();
      prefs.begin("fcs", false);
      prefs.putFloat("magOx", ox); prefs.putFloat("magOy", oy);
      prefs.putFloat("magSx", sx); prefs.putFloat("magSy", sy);
      prefs.putFloat("magXY", sxy);
      prefs.putUChar("magHas", 1);
      prefs.putUChar("layHas", 0);
      prefs.end();
      prefsUnlock();
      Serial.printf("[CAL] OK off=(%.2f,%.2f) W=(%.3f,%.3f,x%.4f) n=%d -> SET NORTH AGAIN\n",
                    (double)ox, (double)oy, (double)sx, (double)sy, (double)sxy, (int)calN);
      calMsg = 1;
    } else {
      Serial.printf("[CAL] FAIL n=%d cover=%d/%d — keep old cal\n",
                    (int)calN, (int)calBinCnt, CAL_BINS);
      calMsg = 2;
    }
    calMsgUntil = millis() + 3000;
  };

  for (;;) {
    // ── คำสั่ง SET_AZ / CAL จาก Core 0 (ผ่านวิทยุ/serial) ──────────
    bool doSetAz = false, doCal = false, doCalClear = false; float setAzDeg = 0;
    bool hdgLive = false;   // v7.4: ทิศตอนนี้เชื่อถือได้ (ตั้งในบล็อกเข็มทิศ — คุมทั้ง valid/ตั้งเหนือ/auto-lay)
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      doSetAz = state.setAzRequest;    state.setAzRequest = false;
      setAzDeg = state.setAzDeg;
      doCal = state.calRequest;        state.calRequest = false;
      doCalClear = state.calClearReq;  state.calClearReq = false;
      xSemaphoreGive(xMutex);
    }

    // ── Compass: RM3100 → one-shot cal → EMA (ไม่มี cal ต่อเนื่อง — เคยทำทิศค้างตอนหัน) ──
    {
      uint32_t nowc = millis();
      bool magStuck = false;

      RM3100Data mag{0, 0, 0};
      // เฟรมใช้ได้ต้อง: อ่านสำเร็จ + ไม่ใช่ 0,0 (อ่านพลาด) + ค่าขยับจากรอบก่อน
      // เงื่อนไข "ขยับ" สำคัญตอน cold boot: ถ้า CMM ไม่สตาร์ท เซนเซอร์ค้างค่าเดิมแต่ DRDY ติด →
      // อ่าน "สำเร็จ" ค่าซ้ำตลอด ถ้านับว่ายังเป็น lastMagOk เด้งเรื่อย → จอขึ้น valid แต่เลขนิ่ง (ค้างถาวร)
      if (compassOk && rm3100_read(mag) && !(mag.x == 0.0f && mag.y == 0.0f)) {
        lastMagOk = nowc;                                 // เซนเซอร์ตอบ SPI + ค่าไม่ใช่ 0,0
        if (prevMagX == 1e9f) {
          // เฟรมแรกหลัง(รี)อาร์ม = baseline เท่านั้น ไม่ตั้งทิศ — ถ้าเป็นค่าค้าง cold boot
          // จะได้ไม่โชว์ทิศผิดเป็น valid อยู่ ~2s ก่อน STUCK จับได้ (เฟรมจริงถัดไปห่างแค่ ~40ms)
          lastMagChange = nowc;
        } else if (mag.x != prevMagX || mag.y != prevMagY) {  // ค่าขยับจริง = CMM กำลังเดิน
          lastMagChange = nowc;
          float hr = headingFromMag(mag) * PI / 180.0f;   // ผ่าน hard/soft-iron cal (ถ้ามี)
          if (!smInit) { smS = sinf(hr); smC = cosf(hr); smInit = true; }
          else {                                          // EMA เวกเตอร์ ตัด spike ไฟรบกวน
            smS += HDG_SMOOTH_A * (sinf(hr) - smS);
            smC += HDG_SMOOTH_A * (cosf(hr) - smC);
          }
          fusedHdg = atan2f(smS, smC) * 180.0f / PI;
          fuseInit = true; hdgFresh = true;   // ทิศมาจากเฟรมจริงของเซสชันเซนเซอร์ปัจจุบันแล้ว
          if (calActive && ++calSkip >= 3) {              // โหมด CAL: เก็บค่าดิบ (ก่อน cal) 1 ใน 3 เฟรม
            calSkip = 0;
            if (calN < CAL_MAX_PTS) { calPts[calN][0] = mag.x; calPts[calN][1] = mag.y; calN++; }
          }
        }
        prevMagX = mag.x; prevMagY = mag.y;
      }
      // ค้าง = อ่านได้แต่ค่าเดิมเป๊ะนานเกิน (CMM ไม่เดิน) — แยกจาก "หลุด" (ไม่ตอบ SPI เลย)
      magStuck = (nowc - lastMagChange) > COMPASS_STUCK_MS;
      if ((nowc - lastMagOk > COMPASS_LOST_MS || magStuck) && nowc - lastReinit > COMPASS_REARM_MS) {
        // หลุด/ค้าง → รีอาร์ม RM3100 เอง (เขียน CMM ใหม่ตอนรางไฟนิ่งแล้ว) ไม่ต้องรีบูต
        lastReinit = nowc;
        compassOk  = rm3100_init();
        prevMagX = prevMagY = 1e9f;   // เฟรมแรกหลังรีอาร์มเป็น baseline ใหม่ (ไม่นับเป็นทิศ)
        hdgFresh = false;             // v7.4: ทิศเดิม = ค่าค้าง ห้าม valid/ตั้งเหนือ จนกว่าจะได้เฟรมขยับจริง
        smS = smC = 0; smInit = false;   // EMA เริ่มนับใหม่จากเฟรมจริงเฟรมแรก ไม่ไหลจากค่าก่อนหลุด
      }
      // v7.4: ทิศเชื่อถือได้จริงไหม — เซนเซอร์ยังตอบ + ค่าไม่ค้าง + fusedHdg มาจากเฟรมจริงหลังรีอาร์มล่าสุด
      // (เดิมเฟรม baseline หลังรีอาร์มก็ปลดกลับ valid ได้ทั้งที่ fusedHdg ยังเป็นทิศเก่า —
      //  จอ/STATUS ออกอากาศทิศค้างเป็น valid ชั่วขณะ และถ้าเซนเซอร์กะพริบได้ยาวถึง 1.5-2s ต่อรอบ)
      hdgLive = (nowc - lastMagOk) < COMPASS_LOST_MS && !magStuck && hdgFresh;

      // auto-lay + เผยแพร่ "ทิศจริง" (true heading = เข็มทิศ + layOffset จาก GPS-track)
      if (fuseInit) {
        while (fusedHdg < 0.0f)      fusedHdg += 360.0f;
        while (fusedHdg >= 360.0f)   fusedHdg -= 360.0f;

        // ── ตั้งทิศเหนืออัตโนมัติ: จับคู่เข็มทิศกับ COG ตอน "วิ่งตรง" ──
        // GPS course = ทิศจริง (true north) ขณะเคลื่อนที่ → offset = COG − ทิศเข็มทิศ
        // เก็บตัวอย่างเป็น circular-mean ทั้งสองค่า แล้วเช็ค resultant length (R) ว่านิ่งจริง
        // (วิ่งตรงไม่เลี้ยว) ค่อยล็อก → กันเคสเลี้ยวกลางทางทำ offset เพี้ยน
        float cog=0, spd=0; bool cogOk=false, readOk=false;
        if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
          cogOk = state.cogValid; cog = state.cog; spd = state.speedKmh;
          xSemaphoreGive(xMutex); readOk = true;
        }
        if (readOk && cogOk && spd >= LAY_MIN_KMH && hdgLive) {   // v7.4: ทิศค้างห้ามจับคู่ COG — offset จะเพี้ยน
          if (laySamp == 0) { layWinStart = nowc; laySpdSum = 0; }
          float cr = cog*PI/180.0f, mr = fusedHdg*PI/180.0f;
          laySinC += sinf(cr); layCosC += cosf(cr);
          laySinM += sinf(mr); layCosM += cosf(mr);
          laySpdSum += spd;
          laySamp++;
          if (nowc - layWinStart >= LAY_WINDOW_MS && laySamp >= LAY_MIN_SAMPLES) {
            float Rc = sqrtf(laySinC*laySinC + layCosC*layCosC) / laySamp;
            float Rm = sqrtf(laySinM*laySinM + layCosM*layCosM) / laySamp;
            if (Rc >= LAY_R_MIN && Rm >= LAY_R_MIN) {     // นิ่งจริง = วิ่งตรงไม่เลี้ยว
              float cogMean = atan2f(laySinC, layCosC) * 180.0f / PI;
              float magMean = atan2f(laySinM, layCosM) * 180.0f / PI;
              float off = cogMean - magMean;
              while (off < -180.0f) off += 360.0f;
              while (off >  180.0f) off -= 360.0f;
              // v7.3: น้ำหนักหน้าต่าง = ความเร็วเฉลี่ย (COG แม่นตามความเร็ว, เพดาน LAY_W_SPD_CAP)
              float w = laySpdSum / laySamp;
              if (w > LAY_W_SPD_CAP) w = LAY_W_SPD_CAP;
              if (!layValid) {
                // v7.3: ล็อกครั้งแรกต้อง 2 หน้าต่างค่าตรงกัน ≤ LAY_AGREE_DEG — หน้าต่างเดียวห้ามฝังค่า
                if (layPendValid) {
                  float dp = off - layPendOff;
                  while (dp < -180.0f) dp += 360.0f;
                  while (dp >  180.0f) dp -= 360.0f;
                  if (fabsf(dp) <= LAY_AGREE_DEG) {       // ตรงกัน → ล็อกด้วยค่าเฉลี่ยถ่วงน้ำหนัก
                    layOffset = layPendOff + dp * (w / (layPendW + w));
                    layWeight = layPendW + w;
                    layValid = true; layFresh = true; laySource = 1;
                    layPendValid = false;
                    Serial.printf("[LAY] GPS-track LOCK off=%.1f (2 windows agree, w=%.0f)\n",
                                  layOffset, layWeight);
                  } else {                                 // ไม่ตรง → ตัวใหม่กลายเป็นตัวรอแทน
                    layPendOff = off; layPendW = w;
                    Serial.printf("[LAY] candidate off=%.1f (disagree %.1f) wait confirm\n",
                                  off, fabsf(dp));
                  }
                } else {
                  layPendOff = off; layPendW = w; layPendValid = true;
                  Serial.printf("[LAY] candidate off=%.1f (w=%.0f) wait confirm window\n", off, w);
                }
              } else {
                // v7.3: อัปเดตถ่วงน้ำหนัก — หลักฐานใหม่เร็ว/นิ่งดึงแรง, สะสมมากแล้วดึงเบา
                float a = w / (layWeight + w);
                if (a < LAY_ALPHA_MIN) a = LAY_ALPHA_MIN;
                if (a > LAY_ALPHA_MAX) a = LAY_ALPHA_MAX;
                float d = off - layOffset;
                while (d < -180.0f) d += 360.0f;
                while (d >  180.0f) d -= 360.0f;
                layOffset += a * d;
                layWeight += w; if (layWeight > LAY_W_MAX) layWeight = LAY_W_MAX;
                layFresh = true; laySource = 1;
                Serial.printf("[LAY] GPS-track off=%.1f (win=%.1f a=%.2f w=%.0f spd=%.0f R=%.3f)\n",
                              layOffset, off, a, layWeight, laySpdSum/laySamp, Rc);
              }
              while (layOffset < 0.0f)      layOffset += 360.0f;
              while (layOffset >= 360.0f)   layOffset -= 360.0f;
            }
            laySinC=layCosC=laySinM=layCosM=0; laySamp=0; // เริ่มหน้าต่างใหม่ (รับ/ไม่รับก็ตาม)
          }
        } else if (readOk && laySamp > 0) {
          laySinC=layCosC=laySinM=layCosM=0; laySamp=0;   // ช้าลง/หยุด → ล้างหน้าต่าง
        }

        // เซฟ offset ลง NVS: ล็อกแรก (NVS ยังไม่มี lay) เซฟทันทีเสมอ — เกณฑ์ >1° ใช้เฉพาะอัปเดตค่าเดิม
        // (v7.4: เดิมล็อกแรกที่ offset<1° ไม่ถูกเซฟ → รีบูตแล้ว lay หายทั้งที่จอเคยขึ้น NSET)
        if (layValid) {
          float ds = layOffset - savedLayOff;
          while (ds < -180.0f) ds += 360.0f;
          while (ds >  180.0f) ds -= 360.0f;
          if (!savedLayHas ||
              (fabsf(ds) > 1.0f && (lastLaySave == 0 || nowc - lastLaySave >= LAY_SAVE_MS))) {
            prefsLock();
            prefs.begin("fcs", false);
            prefs.putFloat("layOff", layOffset); prefs.putUChar("layHas", 1);
            prefs.end();
            prefsUnlock();
            savedLayOff = layOffset; savedLayHas = true; lastLaySave = nowc;
          }
        }

        // ทิศจริงที่เผยแพร่ (ยังไม่ lay = โชว์ค่าเข็มทิศดิบไว้ดูสัมพัทธ์ แต่ห้ามใช้ยิง)
        float trueHdg = fusedHdg + (layValid ? layOffset : 0.0f);
        while (trueHdg < 0.0f)      trueHdg += 360.0f;
        while (trueHdg >= 360.0f)   trueHdg -= 360.0f;
        if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
          state.hdgDeg  = trueHdg;
          state.hdgMils = trueHdg * (6400.0f / 360.0f);
          state.hdgValid = hdgLive;            // หลุด/ค้าง/ยังไม่มีเฟรมจริงหลังรีอาร์ม → false → จอเตือน ไม่ค้างเลขเก่า
          xSemaphoreGive(xMutex);
        }
        if (millis()-lastDebug >= 1000) {
          lastDebug = millis();
          Serial.printf("[HDG] true=%.1f mil=%.0f off=%.1f %s (mag-direct)\n",
                        trueHdg, trueHdg*(6400.0f/360.0f), layOffset, layValid ? "LAID" : "NO-LAY");
          Serial.printf("[MAG] x=%.1f y=%.1f z=%.1f  |xy|=%.1f\n",
                        mag.x, mag.y, mag.z, sqrtf(mag.x*mag.x + mag.y*mag.y));
        }
      }
    }

    // ── โหมด CAL: นับความครอบคลุมรอบทิศทุก 1 วิ + จบอัตโนมัติเมื่อหมุนครบ/หมดเวลา ──
    if (calActive && millis() - lastCalBin >= 1000) {
      lastCalBin = millis();
      if (calN >= 8) {
        float mx = 0, my = 0;                  // ศูนย์กลางคร่าว ๆ = ค่าเฉลี่ยจุดที่เก็บ (พอไว้นับ%;
        for (uint16_t i = 0; i < calN; i++) {  //  ตอนจบเช็คซ้ำด้วยศูนย์จาก fit จริงใน calFinish)
          mx += calPts[i][0]; my += calPts[i][1];
        }
        mx /= calN; my /= calN;
        memset(calBins, 0, sizeof(calBins)); calBinCnt = 0;   // นับใหม่ทั้งชุด (ศูนย์กลางขยับได้)
        for (uint16_t i = 0; i < calN; i++) {
          int bi = (int)((atan2f(calPts[i][1] - my, calPts[i][0] - mx) + PI) / (2.0f * PI) * CAL_BINS);
          if (bi < 0) bi = 0; if (bi >= CAL_BINS) bi = CAL_BINS - 1;
          if (!calBins[bi]) { calBins[bi] = true; calBinCnt++; }
        }
      }
      if ((calBinCnt >= CAL_MIN_BINS && calN >= CAL_MIN_PTS) || millis() - calStart > CAL_TIMEOUT_MS)
        calFinish();
    }

    // ── อ่าน met (BME280) ทุก 2 วิ — อากาศเปลี่ยนช้า, ส่งให้ ศอย. คำนวณ ──
    if (bmeOk && millis()-lastMet >= 2000) {
      lastMet = millis();
      float T, P, H;
      bool ok = bme_read(T, P, H);   // BME บัส Wire(17/18) ร่วม OLED — Core 1 คนเดียว ไม่ต้อง wire1Mutex
      if (ok) {
        if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          state.temp = T; state.press = P; state.hum = H; state.metValid = true;
          xSemaphoreGive(xMutex);
        }
        Serial.printf("[MET] %.1fC  %.1fhPa  %.0f%%RH\n", T, P/100.0f, H);
      }
    }

    // ── คำสั่งจาก serial: CAL / CAL_CLEAR / SET_AZ (ใช้ fusedHdg ปัจจุบัน) ──
    if (doCal && !calActive) calBegin();
    if (doCalClear) {
      bool hadCal = magCalHas;
      magOx = magOy = 0; magSx = magSy = 1; magXY = 0; magCalHas = false;
      if (hadCal) {
        // v7.4: นิยามทิศเปลี่ยน (calibrated→ดิบ) เหมือนตอน CAL OK → lay เดิมผิดความหมาย ต้องล้าง
        // + EMA เริ่มใหม่ (เดิมล้างแค่ cal → จอยังขึ้น NSET ทั้งที่ฐานทิศขยับไปแล้ว = เหนือปลอม)
        layValid = false; layFresh = false; laySource = 0; layOffset = 0;
        savedLayOff = 0; savedLayHas = false; layWeight = 0; layPendValid = false;
        smS = smC = 0; smInit = false;
      }
      prefsLock();
      prefs.begin("fcs", false);
      prefs.putUChar("magHas", 0);
      if (hadCal) prefs.putUChar("layHas", 0);
      prefs.end();
      prefsUnlock();
      Serial.println(hadCal ? "[CAL] cleared — raw heading, SET NORTH AGAIN" : "[CAL] cleared — raw heading");
    }
    if (doSetAz && !calActive) {
      if (hdgLive) applyManualLay(setAzDeg);
      else {        // v7.4: เข็มทิศหลุด/ค้างอยู่ — เดิมรับเงียบ ๆ ด้วยค่าค้าง = ฝังเหนือผิดลง NVS
        layMsg = 1; layMsgUntil = millis() + 2000;
        Serial.println("[LAY] SET_AZ rejected — compass not live");
      }
    }

    // ── Button: ≥2s ปล่อย = ตั้งเหนือ · ≥6s ปล่อย = โหมด CAL 360° · ระหว่าง CAL กด-ปล่อย = ยกเลิก ──
    bool pressed = (digitalRead(BUTTON_PIN)==LOW);
    if (pressed && !btnPrev) btnPressTime = millis();   // เริ่มกด

    if (pressed) {
      uint32_t held = millis()-btnPressTime;
      btnStage = (held >= CAL_HOLD_MS) ? 2 : (held >= SET_NORTH_HOLD_MS) ? 1 : 0;
    } else if (btnPrev) {
      uint32_t held = millis()-btnPressTime;   // ปล่อยปุ่ม
      btnStage = 0;
      if (calActive) {
        if (held >= 100) {                     // กันสัญญาณเด้ง — กดจริงเท่านั้นถึงยกเลิก
          calActive = false; calMsg = 3; calMsgUntil = millis() + 1500;
          Serial.println("[CAL] cancelled");
        }
      }
      else if (held >= CAL_HOLD_MS) calBegin();          // ≥6s = เริ่มคาลิเบรตหมุน 360°
      else if (held >= SET_NORTH_HOLD_MS) {
        if (hdgLive) applyManualLay(0.0f);  // ≥2s = ลำกล้องชี้เหนือ → ตั้งทิศปัจจุบัน = 0°
        else {                              // v7.4: เข็มทิศใช้ไม่ได้ — แจ้ง FAIL ชัด ๆ (เดิมเงียบ เหมือนสำเร็จ)
          layMsg = 1; layMsgUntil = millis() + 2000;
          Serial.println("[LAY] set-north rejected — compass not live");
        }
      }
    }
    btnPrev = pressed;

    // ── Display (100ms refresh) ───────────────────────────────
    if (millis()-lastDisplay >= 100) {
      lastDisplay = millis();

      // Snapshot state
      float   hdgDeg=0, hdgMils=0;
      bool    hdgValid=false, hasCmd=false, gpsValid=false;
      int     azMils=0, elMils=0, charge=1;
      uint8_t sats=0, satsView=0, battPct=0, aimSeq=0;
      double  lat=0, lon=0;
      uint32_t lastFdcRx=0, cmdTime=0;
      int16_t  lastRssi=0;
      float    temp=0; bool metValid=false;

      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        hdgDeg=state.hdgDeg; hdgMils=state.hdgMils; hdgValid=state.hdgValid;
        hasCmd=state.hasCmd; azMils=state.azMils; elMils=state.elMils;
        charge=state.charge; sats=state.sats; satsView=state.satsView;
        gpsValid=state.gpsValid; lat=state.lat; lon=state.lon;
        battPct=state.battPct; lastFdcRx=state.lastFdcRx; lastRssi=state.lastRssi;
        cmdTime=state.cmdTime; aimSeq=state.aimSeq;
        temp=state.temp; metValid=state.metValid;
        xSemaphoreGive(xMutex);
      }
      if (hdgValid) hdgWasValid = true;   // latch: เคยอ่านทิศได้ → แยก "หลุด" ออกจาก "กำลังเริ่ม"

      char buf[32];
      if (calActive) {
        // โหมดคาลิเบรต: % ครอบคลุมรอบทิศ + จำนวนจุด — กด-ปล่อยปุ่ม = ยกเลิก
        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(OLED_X,26, "CAL");
        display.drawStr(OLED_X,44, "360");
        display.setFont(u8g2_font_logisoso20_tr);
        snprintf(buf, 32, "%d%%", (int)calBinCnt * 100 / CAL_BINS);
        display.drawStr(OLED_X,78, buf);
        display.setFont(u8g2_font_6x10_tf);
        snprintf(buf, 32, "n=%d", (int)calN);
        display.drawStr(OLED_X,94, buf);
        display.drawStr(OLED_X,108, "turn slow");
        display.drawStr(OLED_X,120, "btn=stop");
        display.sendBuffer();
      } else if (calMsg && millis() < calMsgUntil) {
        // ผลคาลิเบรตค้างจอ 3 วิ — OK แล้วต้อง "ตั้งเหนือใหม่" เพราะนิยามทิศเปลี่ยน
        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(OLED_X,40, "CAL");
        display.drawStr(OLED_X,58, calMsg == 1 ? "OK" : calMsg == 2 ? "FAIL" : "CANCEL");
        display.setFont(u8g2_font_6x10_tf);
        if (calMsg == 1)      { display.drawStr(OLED_X,84, "Set North"); display.drawStr(OLED_X,96, "again!"); }
        else if (calMsg == 2) display.drawStr(OLED_X,84, "keep old");
        display.sendBuffer();
      } else if (layMsg && millis() < layMsgUntil) {
        // v7.4: กดตั้งเหนือ/SET_AZ ตอนเข็มทิศไม่พร้อม — แจ้งชัดว่าเหนือ "ไม่ถูกตั้ง" (เดิมเงียบหาย)
        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(OLED_X,40, "SET N");
        display.drawStr(OLED_X,58, "FAIL");
        display.setFont(u8g2_font_6x10_tf);
        display.drawStr(OLED_X,84, "compass");
        display.drawStr(OLED_X,96, "not ready");
        display.sendBuffer();
      } else if (btnStage == 2) {
        // กดค้างถึง ≥6s: ปล่อย = เริ่มคาลิเบรต 360°
        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(OLED_X,30, "CAL");
        display.drawStr(OLED_X,48, "360?");
        display.setFont(u8g2_font_6x10_tf);
        display.drawStr(OLED_X,74, "Release");
        display.drawStr(OLED_X,86, "= start");
        display.drawStr(OLED_X,100, "then turn");
        display.drawStr(OLED_X,112, "1 slow rev");
        display.sendBuffer();
      } else if (btnStage == 1) {
        // กดค้าง ≥2s ปล่อย = ตั้งทิศปัจจุบันเป็นเหนือ (จอแนวตั้ง); ค้างต่อถึง 6s = CAL
        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(OLED_X,30, "SET");
        display.drawStr(OLED_X,48, "NORTH?");
        display.setFont(u8g2_font_6x10_tf);
        display.drawStr(OLED_X,74, "Release");
        display.drawStr(OLED_X,86, "= N here");
        display.drawStr(OLED_X,100, "hold 6s");
        display.drawStr(OLED_X,112, "= CAL");
        display.sendBuffer();
      } else {
        // ── จอแนวตั้ง (64×128): หัวข้อมูลแน่น ๆ ด้านบน + โซนทิศตัวใหญ่ด้านล่าง ──
        display.clearBuffer();
        display.setFont(u8g2_font_6x10_tf);

        // [หัว] ปืน · ดาวเทียม · แบต(หรือ USB) · '!' = มี module ดับ (เช่น BME)
        char bb[8];
        if (battPct > 100) snprintf(bb, 8, "USB");
        else               snprintf(bb, 8, "%d%%", battPct);
        snprintf(buf, 32, "G%d S%d %s%s", gunId, sats, bb, bmeOk ? "" : "!");
        display.drawStr(OLED_X,9, buf);

        // [กริด] MGRS แยก 2 บรรทัด: "ZZB SQ" / "EEEE NNNN" (e/n = 9 ตัวท้ายเสมอ ไม่ขึ้นกับ zone 1-2 หลัก)
        if (gpsValid) {
          char mg[24]; latlon_to_mgrs(lat, lon, mg, 24);
          int mglen = strlen(mg);
          if (mglen >= 11) {
            char *en = mg + mglen - 9;        // "1234 5678"
            mg[mglen - 10] = 0;               // ตัดเหลือ "47P AB"
            display.drawStr(OLED_X,19, mg);
            display.drawStr(OLED_X,29, en);
          } else display.drawStr(OLED_X,19, mg);
        } else {
          snprintf(buf, 32, "GPS v%d u%d", satsView, sats);
          display.drawStr(OLED_X,19, buf);
          display.drawStr(OLED_X,29, "acquiring");
        }

        // [ลิงก์ ศอย.] อายุได้ยินล่าสุด + RSSI (บรรทัดเดียว)
        uint32_t age = lastFdcRx ? (millis()-lastFdcRx)/1000 : 0;
        if      (!lastFdcRx) snprintf(buf, 32, "FDC --");
        else if (age <= 30)  snprintf(buf, 32, "FDC %lus %d", (unsigned long)age, lastRssi);
        else                 snprintf(buf, 32, "FDC NOLINK");
        display.drawStr(OLED_X,39, buf);
        display.drawHLine(0, 42, 64);
        display.drawHLine(0, 43, 64);

        // ── โซนปฏิบัติการ (ตัวใหญ่) — เข็มทิศใช้ไม่ได้ต้องเตือนชัด ห้ามโชว์ทิศค้าง/เล็งหลอก ──
        bool compassBad = !compassOk || (!hdgValid && hdgWasValid);
        if (hasCmd) {
          // มีคำสั่งยิง → นำกระบอกเข้าทิศ (ขนาดเลี้ยวตัวใหญ่) + มุมยิง/ดินส่ง
          int azErr = azMils - (int)hdgMils;
          azErr = ((azErr + 3200) % 6400) - 3200;
          bool oriented = hdgValid && layValid;
          bool onTgt = oriented && abs(azErr) <= AZ_THRESHOLD;
          snprintf(buf, 32, "FIRE #%d", aimSeq);
          display.drawStr(OLED_X,56, buf);
          if (compassBad || !hdgValid) {                 // เข็มทิศใช้ไม่ได้ → ห้ามเล็ง
            display.setFont(u8g2_font_8x13B_tf);
            display.drawStr(OLED_X,80, "COMPASS");
            display.drawStr(OLED_X,96, compassBad ? "LOST" : "INIT");
            display.setFont(u8g2_font_6x10_tf);
            snprintf(buf, 32, "AZ%d", azMils); display.drawStr(OLED_X,116, buf);
          } else if (!oriented) {                        // ยังไม่ตั้งเหนือ
            display.setFont(u8g2_font_8x13B_tf);
            display.drawStr(OLED_X,80, "SET N");
            display.drawStr(OLED_X,96, "FIRST");
            display.setFont(u8g2_font_6x10_tf);
            snprintf(buf, 32, "AZ%d", azMils); display.drawStr(OLED_X,116, buf);
          } else if (onTgt) {                            // เข้าเป้า
            display.setFont(u8g2_font_8x13B_tf);
            display.drawStr(OLED_X,80, "ON");
            display.drawStr(OLED_X,96, "TARGET");
            display.setFont(u8g2_font_6x10_tf);
            snprintf(buf, 32, "EL%d C%d", elMils, charge); display.drawStr(OLED_X,116, buf);
          } else {                                       // นำกระบอก: ทิศ (>>R/<<L) + ขนาดตัวใหญ่
            display.drawStr(45, 56, azErr > 0 ? ">>R" : "<<L");   // ชิดขวา (45+18px = 63 พอดีจอ)
            display.setFont(u8g2_font_logisoso20_tr);
            snprintf(buf, 32, "%d", abs(azErr));
            display.drawStr(OLED_X,90, buf);
            display.setFont(u8g2_font_6x10_tf);
            display.drawStr(OLED_X,102, "mil");
            snprintf(buf, 32, "EL%d C%d", elMils, charge); display.drawStr(OLED_X,116, buf);
          }
        } else {
          // ไม่มีคำสั่งยิง → ทิศปัจจุบันตัวใหญ่ (มิล) + สถานะ lay
          if (compassBad || !hdgValid) {                 // เข็มทิศใช้ไม่ได้
            display.setFont(u8g2_font_8x13B_tf);
            if (!compassOk) { display.drawStr(OLED_X,72, "NO"); display.drawStr(OLED_X,90, "COMPASS"); }
            else            { display.drawStr(OLED_X,72, "COMPASS"); display.drawStr(OLED_X,90, compassBad ? "LOST" : "INIT"); }
            display.setFont(u8g2_font_6x10_tf);
            display.drawStr(OLED_X,114, compassBad ? "check wire" : "reading...");
          } else {                                       // ทิศปัจจุบัน ตัวใหญ่
            display.drawStr(OLED_X,56, "HDG");
            display.setFont(u8g2_font_logisoso20_tr);
            snprintf(buf, 32, "%.0f", hdgMils);
            display.drawStr(OLED_X,90, buf);
            display.setFont(u8g2_font_6x10_tf);
            display.drawStr(OLED_X,102, "mil");
            if (layValid) snprintf(buf, 32, "%.0fd %s", hdgDeg, layFresh ? "NSET" : "NSET?");
            else          snprintf(buf, 32, "%.0fd DRV", hdgDeg);
            display.drawStr(OLED_X,116, buf);
          }
        }
        display.sendBuffer();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200); delay(500);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  prefs.begin("fcs", false);
  gunId = prefs.getUChar("gunId", 1);
  magCalHas = prefs.getUChar("magHas", 0) ? true : false;   // hard/soft-iron cal จากโหมด CAL 360°
  if (magCalHas) {
    magOx = prefs.getFloat("magOx", 0); magOy = prefs.getFloat("magOy", 0);
    magSx = prefs.getFloat("magSx", 1); magSy = prefs.getFloat("magSy", 1);
    magXY = prefs.getFloat("magXY", 0);   // cal เก่า (ก่อน v7.4) ไม่มี key นี้ → 0 = สูตรเดิมเป๊ะ
  }
  prefs.end();
  cryptInit();   // โหลด key + counter จาก NVS ก่อนใช้วิทยุ

  // Hold BOOT at boot = change Gun ID
  if (digitalRead(BUTTON_PIN)==LOW) {
    delay(600);
    if (digitalRead(BUTTON_PIN)==LOW) {
      gunId = (gunId%4)+1;
      prefs.begin("fcs",false); prefs.putUChar("gunId",gunId); prefs.end();
    }
  }

  // OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(); display.setFont(u8g2_font_6x10_tf);
  char buf[32];
  display.clearBuffer();
  display.drawStr(OLED_X,12, "Artillery");
  display.drawStr(OLED_X,24, "FCS v7");
  snprintf(buf, 32, "Gun #%d", gunId);
  display.drawStr(OLED_X,44, buf);
  display.drawStr(OLED_X,60, "Init...");
  display.sendBuffer();

  // PMU
  Wire1.begin(I2C1_SDA, I2C1_SCL);
  pmuOk = pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, I2C1_SDA, I2C1_SCL);
  pmu.setALDO3Voltage(3300); pmu.enableALDO3();
  pmu.setALDO4Voltage(3300); pmu.enableALDO4();
  pmu.setALDO1Voltage(3300); pmu.enableALDO1();
  pmu.setALDO2Voltage(3300); pmu.enableALDO2();
  pmu.setBLDO1Voltage(3300); pmu.enableBLDO1();
  delay(300);

  // GPS
  pinMode(GPS_EN_PIN, OUTPUT); digitalWrite(GPS_EN_PIN, HIGH); delay(100);
  gpsSerial.setRxBufferSize(8192);   // เผื่อกว้าง: v7.2 ส่งแบบไม่ทิ้ง GPS แล้ว แต่กันไว้เผื่อเคสอื่นบล็อก
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // LoRa
  SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
  int radioState = radio.begin();
  while (radioState != RADIOLIB_ERR_NONE) {
    // v7.2: เดิมวนตายเงียบ ๆ จอค้าง "Init..." — เปลี่ยนเป็นเตือนบนจอ + retry
    // (เผื่อรางไฟ SX1262 ยังไม่นิ่งตอน cold boot แล้วรอบถัดไปสำเร็จ)
    Serial.printf("[RADIO] FAIL code=%d retry...\n", radioState);
    display.clearBuffer();
    display.setFont(u8g2_font_8x13B_tf);
    display.drawStr(OLED_X,40, "RADIO");
    display.drawStr(OLED_X,58, "FAIL");
    display.setFont(u8g2_font_6x10_tf);
    snprintf(buf, 32, "err %d", radioState);
    display.drawStr(OLED_X,80, buf);
    display.drawStr(OLED_X,100, "retrying");
    display.sendBuffer();
    delay(1000);
    radioState = radio.begin();
  }
  radio.setFrequency(LORA_FREQ); radio.setBandwidth(LORA_BW);
  radio.setSpreadingFactor(LORA_SF); radio.setCodingRate(LORA_CR);
  radio.setSyncWord(LORA_SYNC); radio.setOutputPower(LORA_POWER);
  radio.setCurrentLimit(140); radio.setPreambleLength(LORA_PREAMBLE);
  radio.setCRC(2); radio.setDio1Action(setRxFlag); radio.startReceive();
  Serial.printf("[LoRa] OK %.1fMHz CH%d AES-CCM\n", LORA_FREQ, LORA_CHANNEL);

  // RM3100
  compassOk = rm3100_init();
  Serial.printf("[RM3100] %s\n", compassOk ? "OK" : "FAIL");

  // I2C scan ทั้งสองบัส (ยืนยันว่าบอร์ดมีอะไรจริง) + BME280 อากาศ
  i2cScan(Wire,  "OLED");
  i2cScan(Wire1, "sensor");
  bmeOk = bme_init();
  Serial.printf("[BME280] %s @0x%02X %s\n",
                bmeOk ? "OK" : "not found", bmeAddr, bmeHasHum ? "(T/P/H)" : "(T/P)");

  Serial.printf("[GUN%d] Ready — dual-core FreeRTOS\n", gunId);

  // ── SELF-TEST (จอแนวตั้ง): สถานะ module ทีละบรรทัด ยืนยัน hardware ก่อนเริ่มงาน ──
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(OLED_X,10, "SELF-TEST");
  snprintf(buf, 32, "Gun #%d", gunId);
  display.drawStr(OLED_X,22, buf);
  display.drawHLine(0, 25, 64);
  snprintf(buf, 32, "MAG  %s%s", compassOk ? "OK" : "FAIL", magCalHas ? "+C" : "");   // RM3100 (+C = มี cal 360°)
  display.drawStr(OLED_X,39, buf);
  display.drawStr(OLED_X,51, "LoRa OK");
  snprintf(buf, 32, "AIR  %s", bmeOk ? "OK" : "--");          // BME280
  display.drawStr(OLED_X,63, buf);
  snprintf(buf, 32, "PMU  %s", pmuOk ? "OK" : "FAIL");
  display.drawStr(OLED_X,75, buf);
  display.drawStr(OLED_X,87, "GPS  wait");
  display.drawHLine(0, 90, 64);
  { prefs.begin("fcs", true);
    bool hasLay = prefs.getUChar("layHas", 0);
    float layO  = prefs.getFloat("layOff", 0.0f);
    prefs.end();
    if (hasLay) { snprintf(buf, 32, "N-SET %.0f", (double)layO); display.drawStr(OLED_X,104, buf); }
    else        { display.drawStr(OLED_X,104, "North:"); display.drawStr(OLED_X,116, "DRIVE=N"); } }
  display.sendBuffer();
  delay(2500);

  // Create mutexes
  xMutex = xSemaphoreCreateMutex();
  wire1Mutex = xSemaphoreCreateMutex();
  prefsMutex = xSemaphoreCreateMutex();

  // Create tasks
  xTaskCreatePinnedToCore(taskRadioGPS,      "RadioGPS", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(taskCompassDisplay,"CompassUI", 8192, NULL, 1, NULL, 1);
}

void loop() {
  // Empty — FreeRTOS tasks handle everything
  vTaskDelay(portMAX_DELAY);
}
