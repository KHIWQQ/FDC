/*
 * Artillery FCS — Phase 2
 * GUN UNIT firmware v7 (T-Beam SUPREME)
 * FreeRTOS dual-core architecture:
 *   Core 0: GPS + LoRa RX/TX + Status broadcast
 *   Core 1: Compass (RM3100) + Display + Button
 * RM3100 SPI compass — no calibration needed
 * Hold BOOT 2s = set north reference
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
#define RM_CS             6

// ── LoRa Config — ย่าน AS923 ไทย (กสทช. 920–925 MHz) ────────────
// STEP B1: ตารางช่อง — ต้องตั้ง LORA_CHANNEL ตรงกับ fdc_dongle ของกองร้อยเดียวกัน
static const float LORA_CHANNELS[] = {
  920.6, 921.2, 921.8, 922.4, 923.0, 923.6, 924.2, 924.8   // CH0–CH7, ห่าง 600kHz
};
#define LORA_CHANNEL    4          // CH4 = 923.0 MHz (กลางย่าน)
#define LORA_FREQ     (LORA_CHANNELS[LORA_CHANNEL])
#define LORA_BW        62.5
#define LORA_SF        12
#define LORA_CR         5
#define LORA_SYNC    0xAB
#define LORA_POWER     22
#define LORA_PREAMBLE   8
#define AZ_THRESHOLD    15
#define NORTH_SET_HOLD_MS 2000

// ── RM3100 Registers ─────────────────────────────────────────────
#define RM3100_REG_CMM    0x01
#define RM3100_REG_CCX    0x04
#define RM3100_REG_CCY    0x06
#define RM3100_REG_CCZ    0x08
#define RM3100_REG_MX     0x24
#define RM3100_REG_STATUS 0x34
#define RM3100_REVID      0x36
#define RM3100_CYCLE      200
#define RM3100_SCALE      (1.0f / (0.3671f * sqrtf(RM3100_CYCLE)))

// ── RM3100 struct (must be before functions) ─────────────────────
struct RM3100Data { float x, y, z; };

// ── Shared State (protected by mutex) ───────────────────────────
SemaphoreHandle_t xMutex;

struct SharedState {
  // GPS
  double  lat = 0, lon = 0;
  float   alt = 0;
  uint8_t sats = 0;
  bool    gpsValid = false;
  // Heading
  float   hdgRaw = 0, hdgDeg = 0, hdgMils = 0;
  bool    hdgValid = false;
  // Aim command
  int     azMils = 0, elMils = 0, charge = 1;
  uint8_t aimSeq = 0;
  bool    hasCmd = false;
  uint32_t cmdTime = 0;
  // Battery
  uint8_t battPct = 0;
  // North offset
  float   northOffset = 0;
  bool    northSetRequest = false;
} state;

// ── Hardware objects ─────────────────────────────────────────────
SPIClass rm_spi(HSPI);
XPowersAXP2101 pmu;
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
Preferences prefs;

uint8_t gunId = 1;
bool compassOk = false;

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
    prefs.begin("fcs", false); prefs.putUInt("loractr", txCtr); prefs.end();
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
  prefs.begin("fcs", false);
  prefs.putBytes("lorakey", loraKey, 16);
  prefs.end();
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

// ── LoRa transmit (blocking but only on Core 0) ──────────────────
// STEP B2: เข้ารหัสทุกแพ็กเก็ตก่อนออกอากาศ (sender = gunId)
void loraSend(uint8_t *data, size_t len) {
  uint8_t env[80];
  size_t n = cryptSeal(gunId, data, len, env);
  if (n) radio.transmit(env, n);
  radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════
// CORE 0 TASK — GPS + LoRa
// ═══════════════════════════════════════════════════════════════
void taskRadioGPS(void *pv) {
  uint32_t lastStatus = 0;
  uint32_t statusInterval = 5000;

  for (;;) {
    // GPS
    while (gpsSerial.available()) gps.encode(gpsSerial.read());

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      if (gps.location.isValid()) {
        state.lat = gps.location.lat();
        state.lon = gps.location.lng();
        state.alt = gps.altitude.meters();
        state.gpsValid = true;
      }
      state.sats = gps.satellites.value();
      state.battPct = (uint8_t)constrain(
        map(pmu.getBattVoltage(), 3300, 4200, 0, 100), 0, 100);
      xSemaphoreGive(xMutex);
    }

    // Serial config (provisioning key ผ่าน USB)
    handleSerialCfg();

    // LoRa RX
    if (loraRxFlag) {
      loraRxFlag = false;
      uint8_t env[80], buf[64];
      int st = radio.readData(env, sizeof(env));
      if (st == RADIOLIB_ERR_NONE) {
        size_t envLen = radio.getPacketLength();
        // STEP B2: ถอดรหัส + MAC + กัน replay — รับเฉพาะแม่ข่าย (SENDER_FDC)
        uint8_t sender = 0;
        size_t len = cryptOpen(env, envLen, buf, &sender);
        if (len >= 4 && sender == SENDER_FDC) {
          uint16_t rxCrc = ((uint16_t)buf[len-2]<<8)|buf[len-1];
          if (rxCrc == crc16(buf, len-2)) {
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
  bool btnHeld = false;
  bool btnPrev = false;
  bool showNorthPrompt = false;
  float northOffset = 0;

  // Load north offset
  if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
    northOffset = state.northOffset;
    xSemaphoreGive(xMutex);
  }

  for (;;) {
    // ── Compass ──────────────────────────────────────────────
    if (compassOk) {
      RM3100Data mag;
      if (rm3100_read(mag)) {
        float deg = atan2(mag.x, -mag.y) * 180.0f / PI;
        if (deg < 0) deg += 360.0f;
        float raw = deg;
        float corrected = raw - northOffset;
        if (corrected < 0)      corrected += 360.0f;
        if (corrected >= 360.0f) corrected -= 360.0f;

        if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
          state.hdgRaw  = raw;
          state.hdgDeg  = corrected;
          state.hdgMils = corrected * (6400.0f / 360.0f);
          state.hdgValid = true;
          xSemaphoreGive(xMutex);
        }

        // Serial debug every 1s
        if (millis()-lastDebug >= 1000) {
          lastDebug = millis();
          Serial.printf("[HDG] raw=%.1f corrected=%.1f deg=%.0f mil=%.0f\n",
                        raw, corrected, corrected, corrected*(6400.0f/360.0f));
        }
      }
    }

    // ── Button ───────────────────────────────────────────────
    bool pressed = (digitalRead(BUTTON_PIN)==LOW);
    if (pressed && !btnPrev) {
      btnHeld = true;
      btnPressTime = millis();
      showNorthPrompt = false;
    }
    if (!pressed && btnPrev) {
      btnHeld = false;
      showNorthPrompt = false;
    }
    btnPrev = pressed;

    if (btnHeld) {
      uint32_t held = millis()-btnPressTime;
      if (held > 500) showNorthPrompt = true;
      if (held >= NORTH_SET_HOLD_MS) {
        btnHeld = false;
        showNorthPrompt = false;

        RM3100Data mag;
        float raw = 0;
        if (compassOk && rm3100_read(mag)) {
          raw = atan2(mag.x, -mag.y) * 180.0f / PI;
          if (raw < 0) raw += 360.0f;
        } else {
          if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            raw = state.hdgRaw;
            xSemaphoreGive(xMutex);
          }
        }
        northOffset = raw;

        // Save to NVS and state
        prefs.begin("fcs", false);
        prefs.putFloat("northOff", raw);
        prefs.end();
        if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          state.northOffset = raw;
          state.hdgDeg  = 0;
          state.hdgMils = 0;
          xSemaphoreGive(xMutex);
        }
        Serial.printf("[NORTH] SET! raw=%.1f\n", raw);

        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(0, 20, "NORTH SET!");
        display.setFont(u8g2_font_6x10_tf);
        char buf[32];
        snprintf(buf, 32, "offset: %.1f deg", raw);
        display.drawStr(0, 36, buf);
        display.drawStr(0, 50, "Barrel = 0 mil");
        display.sendBuffer();
        vTaskDelay(pdMS_TO_TICKS(1500));
      }
    }

    // ── Display (100ms refresh) ───────────────────────────────
    if (millis()-lastDisplay >= 100) {
      lastDisplay = millis();

      // Snapshot state
      float   hdgDeg=0, hdgMils=0;
      bool    hdgValid=false, hasCmd=false, gpsValid=false;
      int     azMils=0, elMils=0, charge=1;
      uint8_t sats=0;
      double  lat=0, lon=0;

      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        hdgDeg=state.hdgDeg; hdgMils=state.hdgMils; hdgValid=state.hdgValid;
        hasCmd=state.hasCmd; azMils=state.azMils; elMils=state.elMils;
        charge=state.charge; sats=state.sats;
        gpsValid=state.gpsValid; lat=state.lat; lon=state.lon;
        xSemaphoreGive(xMutex);
      }

      if (showNorthPrompt) {
        uint32_t held = millis()-btnPressTime;
        uint32_t rem = (held < NORTH_SET_HOLD_MS) ? (NORTH_SET_HOLD_MS-held)/1000+1 : 0;
        display.clearBuffer();
        display.setFont(u8g2_font_8x13B_tf);
        display.drawStr(0, 20, "SET NORTH...");
        display.setFont(u8g2_font_6x10_tf);
        display.drawStr(0, 36, "Point barrel");
        display.drawStr(0, 48, "to NORTH");
        char buf[24]; snprintf(buf, 24, "Release in %lus", rem);
        display.drawStr(0, 62, buf);
        display.sendBuffer();
      } else {
        display.clearBuffer();
        display.setFont(u8g2_font_6x10_tf);
        char buf[32];

        snprintf(buf, 32, "GUN#%d S:%d %s", gunId, sats, hdgValid?"C:OK":"C:--");
        display.drawStr(0, 10, buf);

        if (gpsValid) snprintf(buf, 32, "%.4f,%.4f", lat, lon);
        else          snprintf(buf, 32, "GPS: No Fix (%d)", sats);
        display.drawStr(0, 20, buf);
        display.drawHLine(0, 23, 128);

        if (!hasCmd) {
          if (hdgValid) {
            snprintf(buf, 32, "HDG: %.0f deg", hdgDeg);
            display.drawStr(0, 35, buf);
            snprintf(buf, 32, "    %.0f mil", hdgMils);
            display.drawStr(0, 47, buf);
          } else {
            display.drawStr(0, 35, "Compass: --");
          }
          display.drawStr(0, 59, "Hold BOOT=SetNorth");
        } else {
          int azErr = azMils - (int)hdgMils;
          azErr = ((azErr + 3200) % 6400) - 3200;
          bool onTgt = hdgValid && abs(azErr) <= AZ_THRESHOLD;

          if (onTgt) {
            display.setFont(u8g2_font_8x13B_tf);
            display.drawStr(0, 38, "** ON TARGET **");
          } else if (hdgValid) {
            display.setFont(u8g2_font_8x13B_tf);
            if (azErr>0) snprintf(buf, 32, ">>> R %d mil", azErr);
            else         snprintf(buf, 32, "<<< L %d mil", -azErr);
            display.drawStr(0, 38, buf);
          } else {
            display.setFont(u8g2_font_8x13B_tf);
            snprintf(buf, 32, "AZ:%4d mil", azMils);
            display.drawStr(0, 38, buf);
          }
          display.setFont(u8g2_font_6x10_tf);
          snprintf(buf, 32, "EL:%4d CHG:%d", elMils, charge);
          display.drawStr(0, 52, buf);
          snprintf(buf, 32, "HDG:%.0f mil", hdgMils);
          display.drawStr(0, 62, buf);
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
  state.northOffset = prefs.getFloat("northOff", 0.0f);
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
  char buf[32]; snprintf(buf,32,"Gun #%d v7",gunId);
  display.clearBuffer();
  display.drawStr(0,14,"Artillery FCS v7");
  display.drawStr(0,28,buf);
  display.drawStr(0,42,"Init...");
  display.sendBuffer();

  // PMU
  Wire1.begin(I2C1_SDA, I2C1_SCL);
  pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, I2C1_SDA, I2C1_SCL);
  pmu.setALDO3Voltage(3300); pmu.enableALDO3();
  pmu.setALDO4Voltage(3300); pmu.enableALDO4();
  pmu.setALDO1Voltage(3300); pmu.enableALDO1();
  pmu.setALDO2Voltage(3300); pmu.enableALDO2();
  pmu.setBLDO1Voltage(3300); pmu.enableBLDO1();
  delay(300);

  // GPS
  pinMode(GPS_EN_PIN, OUTPUT); digitalWrite(GPS_EN_PIN, HIGH); delay(100);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // LoRa
  SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
  int radioState = radio.begin();
  if (radioState != RADIOLIB_ERR_NONE) {
    Serial.printf("[RADIO] FAIL code=%d\n", radioState);
    while(1) delay(1000);
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

  Serial.printf("[GUN%d] Ready — dual-core FreeRTOS\n", gunId);

  display.clearBuffer();
  snprintf(buf,32,"Gun #%d READY",gunId);
  display.drawStr(0,14,"Artillery FCS v7");
  display.drawStr(0,28,buf);
  display.drawStr(0,42,compassOk?"RM3100: OK":"RM3100: FAIL");
  snprintf(buf,32,"N-off: %.0f", state.northOffset);
  display.drawStr(0,56,buf);
  display.sendBuffer();
  delay(1000);

  // Create mutex
  xMutex = xSemaphoreCreateMutex();

  // Create tasks
  xTaskCreatePinnedToCore(taskRadioGPS,      "RadioGPS", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(taskCompassDisplay,"CompassUI", 8192, NULL, 1, NULL, 1);
}

void loop() {
  // Empty — FreeRTOS tasks handle everything
  vTaskDelay(portMAX_DELAY);
}
