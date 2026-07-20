/*
 * ============================================================
 *  Artillery FCS — Phase 2
 *  FDC DONGLE firmware v2 (T-Beam SUPREME #1)
 *  Pin map: LILYGO official + PMU init
 * ============================================================
 *  Libraries: RadioLib, TinyGPSPlus, ArduinoJson, U8g2, XPowersLib
 * ============================================================
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
#include <mbedtls/md.h>

// LoRa SX1262
#define RADIO_SCLK_PIN   12
#define RADIO_MISO_PIN   13
#define RADIO_MOSI_PIN   11
#define RADIO_CS_PIN     10
#define RADIO_RST_PIN     5
#define RADIO_DIO1_PIN    1
#define RADIO_BUSY_PIN    4
// OLED I2C
#define I2C_SDA          17
#define I2C_SCL          18
// PMU I2C
#define I2C1_SDA         42
#define I2C1_SCL         41
// GPS
#define GPS_RX_PIN        9
#define GPS_TX_PIN        8
#define GPS_EN_PIN        7
#define GPS_BAUD      9600
// Button
#define BUTTON_PIN        0

// LoRa Config — ย่าน AS923 ไทย (กสทช. 920–925 MHz, ≤500mW EIRP ไม่ต้องขอใบอนุญาต)
// STEP B1: ตารางช่องสำหรับวางแผนความถี่หลายหน่วย — กองร้อยข้างเคียงใช้คนละช่องกันชนกัน
// ทุก node ในกองร้อยเดียวกัน (dongle + gun units) ต้องตั้ง LORA_CHANNEL ตรงกัน
static const float LORA_CHANNELS[] = {
  920.6, 921.2, 921.8, 922.4, 923.0, 923.6, 924.2, 924.8   // CH0–CH7, ห่าง 600kHz
};
#define LORA_CHANNEL    4          // CH4 = 923.0 MHz (กลางย่าน)
#define LORA_FREQ     (LORA_CHANNELS[LORA_CHANNEL])
#define LORA_BW       125.0     // v7.2 คู่ gun_unit: 62.5→125 + SF12→9 — airtime ลด ~14 เท่า
#define LORA_SF         9       // *** ต้องตรงกับ gun_unit ทุกกระบอก ไม่งั้นลิงก์เงียบสนิท ***
#define LORA_CR         5
#define LORA_SYNC    0xAB
#define LORA_POWER     22
#define LORA_PREAMBLE   8
#define NODE_FDC      0x01

XPowersAXP2101 pmu;
Preferences prefs;
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

struct FdcGpsState {
  double lat = 0, lon = 0;
  float  alt = 0;
  uint8_t fix = 0, sats = 0;
  bool valid = false;
} fdcGps;

struct GunStatus {
  bool active = false;
  double lat = 0, lon = 0;
  int azMils = 0, elMils = 0;
  uint8_t bat = 0;
  int16_t rssi = 0;
  float snr = 0;
  uint32_t lastSeen = 0;
} guns[5];

// แบตของ dongle เอง (ประกาศไว้บนสุดเพื่อให้ Arduino สร้าง prototype ของ readDongleBat() ได้)
struct DongleBat { int pct; float volt; bool charging; bool vbus; bool present; };

uint32_t lastGpsBcast = 0, lastDisplay = 0;
uint8_t txSeq = 0;
volatile bool loraRxFlag = false;
String serialBuf = "";

void IRAM_ATTR setRxFlag() { loraRxFlag = true; }

uint16_t crc16(const uint8_t *d, size_t l) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < l; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int j = 0; j < 8; j++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

// ============================================================
//  STEP B2 — LoRa link crypto: AES-128-CCM + anti-replay
//  Envelope: [0]=0xE1 magic | [1..4]=counter BE | [5]=sender
//            | ciphertext (= แพ็กเก็ตเดิมรวม CRC16) | tag 8B
//  header 6B เป็น AAD (ถูก authenticate แต่ไม่เข้ารหัส)
//  anti-replay: รับเฉพาะ counter ที่มากกว่าค่าล่าสุดของ sender นั้น
// ============================================================
#define CRYPT_MAGIC    0xE1
#define CRYPT_HDR_LEN  6
#define CRYPT_TAG_LEN  8
#define SENDER_FDC     0xF1   // sender id ของแม่ข่าย (แยกจาก gun id 1–4)

// คีย์เริ่มต้น "FDC-DEFAULT-KEY!" — ใช้ทดสอบเท่านั้น ก่อนใช้จริงต้องตั้งใหม่
// ผ่าน serial: {"type":"SET_KEY","key":"<hex 32 ตัว>"} → เก็บใน NVS
static uint8_t  loraKey[16] = {
  0x46,0x44,0x43,0x2D,0x44,0x45,0x46,0x41,
  0x55,0x4C,0x54,0x2D,0x4B,0x45,0x59,0x21
};
static uint32_t txCtr = 0;
static uint32_t lastRxCtr[256] = {0};

void cryptInit() {
  prefs.begin("fcs", false);
  prefs.getBytes("lorakey", loraKey, 16);          // ถ้าไม่เคยตั้ง ใช้ default เดิม
  // counter persist ทุก 64 ครั้ง — boot ใหม่ข้ามไป +64 การันตีไม่ซ้ำ nonce เดิม
  txCtr = prefs.getUInt("loractr", 0) + 64;
  prefs.putUInt("loractr", txCtr);
  prefs.end();
}

static void makeNonce(uint8_t sender, uint32_t ctr, uint8_t nonce[13]) {
  memset(nonce, 0, 13);
  nonce[0] = sender;
  nonce[1] = (ctr >> 24) & 0xFF; nonce[2] = (ctr >> 16) & 0xFF;
  nonce[3] = (ctr >>  8) & 0xFF; nonce[4] =  ctr        & 0xFF;
}

// เข้ารหัส plain[len] → out, คืนความยาว envelope (0 = ล้มเหลว)
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
             out, CRYPT_HDR_LEN,                       // AAD = header
             plain, out + CRYPT_HDR_LEN,
             out + CRYPT_HDR_LEN + len, CRYPT_TAG_LEN);
  mbedtls_ccm_free(&c);
  return rc == 0 ? CRYPT_HDR_LEN + len + CRYPT_TAG_LEN : 0;
}

// ถอดรหัส buf[len] → out, คืนความยาว plaintext (0 = ปลอม/replay/พัง)
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
  lastRxCtr[sender] = ctr;                             // อัปเดตหลัง MAC ผ่านเท่านั้น
  if (senderOut) *senderOut = sender;
  return plen;
}

// ส่งแบบเข้ารหัส (แทน radio.transmit ตรงๆ ทุกจุด)
void secureTransmit(const uint8_t *plain, size_t len) {
  uint8_t env[80];
  size_t n = cryptSeal(SENDER_FDC, plain, len, env);
  if (n) radio.transmit(env, n);
  radio.startReceive();
}

void setLoraKey(const char *hex) {
  if (!hex || strlen(hex) != 32) {
    Serial.println("{\"type\":\"KEY_ERR\",\"msg\":\"need 32 hex chars\"}");
    return;
  }
  for (int i = 0; i < 16; i++) {
    char b[3] = { hex[i*2], hex[i*2+1], 0 };
    loraKey[i] = (uint8_t)strtol(b, nullptr, 16);
  }
  prefs.begin("fcs", false);
  prefs.putBytes("lorakey", loraKey, 16);
  prefs.end();
  Serial.println("{\"type\":\"KEY_OK\"}");
}

// HMAC-SHA256(loraKey, msg) → hex 32 ตัวแรก (ใช้ตอบ challenge จาก PC)
void hmacHex(const char *msg, char out[33]) {
  uint8_t mac[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, loraKey, 16, (const uint8_t *)msg, strlen(msg), mac);
  for (int i = 0; i < 16; i++) sprintf(out + i*2, "%02x", mac[i]);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  cryptInit();   // โหลด key + counter จาก NVS ก่อนใช้วิทยุ

  // PMU — ต้อง init ก่อน LoRa และ GPS จะทำงาน
  Wire1.begin(I2C1_SDA, I2C1_SCL);
  if (!pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, I2C1_SDA, I2C1_SCL)) {
    Serial.println("{\"type\":\"ERROR\",\"msg\":\"PMU failed\"}");
    while (true) delay(1000);
  }
  pmu.setALDO3Voltage(3300); pmu.enableALDO3(); // LoRa
  pmu.setALDO4Voltage(3300); pmu.enableALDO4(); // GPS
  pmu.setALDO1Voltage(3300); pmu.enableALDO1();
  pmu.setALDO2Voltage(3300); pmu.enableALDO2();
  pmu.setBLDO1Voltage(3300); pmu.enableBLDO1();
  delay(200);

  // GPS
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);
  gpsSerial.setRxBufferSize(4096);   // กัน buffer ล้นช่วง LoRa ส่ง blocking ~3s
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin();
  display.setFont(u8g2_font_6x10_tf);
  display.clearBuffer();
  display.drawStr(0, 14, "FDC Dongle v2");
  display.drawStr(0, 28, "Initializing...");
  display.sendBuffer();

  // LoRa
  SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
  int state = radio.begin();
  while (state != RADIOLIB_ERR_NONE) {
    // เดิมวนตายเงียบ ๆ จอค้าง "Initializing..." — เปลี่ยนเป็นเตือนบนจอ + retry
    Serial.printf("{\"type\":\"ERROR\",\"msg\":\"LoRa failed %d\"}\n", state);
    display.clearBuffer();
    display.setFont(u8g2_font_8x13B_tf);
    display.drawStr(0, 24, "RADIO FAIL");
    display.setFont(u8g2_font_6x10_tf);
    char eb[24]; snprintf(eb, 24, "err %d retrying...", state);
    display.drawStr(0, 44, eb);
    display.sendBuffer();
    delay(1000);
    state = radio.begin();
  }
  radio.setFrequency(LORA_FREQ);
  radio.setBandwidth(LORA_BW);
  radio.setSpreadingFactor(LORA_SF);
  radio.setCodingRate(LORA_CR);
  radio.setSyncWord(LORA_SYNC);
  radio.setOutputPower(LORA_POWER);
  radio.setCurrentLimit(140);
  radio.setPreambleLength(LORA_PREAMBLE);
  radio.setCRC(2);
  radio.setDio1Action(setRxFlag);
  radio.startReceive();

  display.clearBuffer();
  display.drawStr(0, 14, "FDC Dongle v3");
  char fbuf[32];
  snprintf(fbuf, 32, "LoRa %.1fMHz CH%d", LORA_FREQ, LORA_CHANNEL);
  display.drawStr(0, 28, fbuf);
  display.drawStr(0, 42, "SEC: AES-CCM");
  display.drawStr(0, 56, "GPS Acquiring...");
  display.sendBuffer();

  Serial.println("{\"type\":\"INFO\",\"msg\":\"FDC Dongle ready v3\"}");
  // พ่น IDENT ซ้ำ 3 ครั้งตอน boot (ห่างกัน 200ms) กันฝั่ง PC พลาดตอนเพิ่งเสียบ
  for (int i = 0; i < 3; i++) { sendIdent(nullptr); delay(200); }
}

// ตอบยืนยันตัวตน — ตอน boot (ไม่มี nonce) และเมื่อ PC ส่ง WHOAMI มาถาม
// ถ้า PC แนบ nonce มา ตอบ hmac = HMAC-SHA256(loraKey, nonce) ด้วย
// → PC ที่ตั้ง psk ไว้ตรวจได้ว่าเป็น dongle ตัวจริง ไม่ใช่อุปกรณ์เลียน JSON
void sendIdent(const char *nonce) {
  StaticJsonDocument<256> doc;
  doc["type"]  = "IDENT";
  doc["role"]  = "FDC_DONGLE";
  doc["fw"]    = "v3";
  doc["proto"] = 2;                  // proto 2 = encrypted LoRa (AES-CCM)
  doc["node"]  = 1;
  doc["freq"]  = LORA_FREQ;
  doc["ch"]    = LORA_CHANNEL;
  if (nonce && strlen(nonce) > 0) {
    char h[33];
    hmacHex(nonce, h);
    doc["hmac"] = h;
  }
  serializeJson(doc, Serial);
  Serial.println();
}

// อ่านแบตของ dongle เอง (T-Beam มี PMU AXP2101)
DongleBat readDongleBat() {
  uint16_t mv = pmu.getBattVoltage();        // แรงดันแบต (mV) — 0 ถ้าไม่มีแบต
  DongleBat b;
  b.present  = mv > 2500;                     // ต่ำกว่านี้ถือว่าไม่มีแบต/เสียบ USB อย่างเดียว
  b.volt     = mv / 1000.0f;
  b.pct      = constrain(map(mv, 3300, 4200, 0, 100), 0, 100);
  b.charging = pmu.isCharging();
  b.vbus     = pmu.isVbusIn();               // มีไฟ USB เข้าหรือไม่
  return b;
}

void loop() {
  readGps();
  handleLoraRx();
  handleSerialRx();
  broadcastGps();
  refreshDisplay();
}

void readGps() {
  while (gpsSerial.available()) {
    if (gps.encode(gpsSerial.read())) {
      fdcGps.sats = gps.satellites.value();
      if (gps.location.isValid()) {
        fdcGps.lat = gps.location.lat();
        fdcGps.lon = gps.location.lng();
        fdcGps.alt = gps.altitude.meters();
        fdcGps.fix = 3;
        fdcGps.valid = true;
      }
    }
  }
}

void broadcastGps() {
  if (millis() - lastGpsBcast < 2000) return;
  lastGpsBcast = millis();
  StaticJsonDocument<200> doc;
  doc["type"] = "FDC_GPS";
  doc["lat"]  = fdcGps.lat;
  doc["lon"]  = fdcGps.lon;
  doc["alt"]  = (int)fdcGps.alt;
  doc["fix"]  = fdcGps.fix;
  doc["sats"] = fdcGps.sats;
  DongleBat b = readDongleBat();             // แนบแบต dongle ไปกับ heartbeat เผื่อหน้าเว็บใช้
  doc["bat"]  = b.present ? b.pct : -1;       // -1 = ไม่มีแบต/รันบน USB
  doc["chg"]  = b.charging;
  serializeJson(doc, Serial);
  Serial.println();
}

void handleSerialRx() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialBuf.trim();
      if (serialBuf.length() > 0) processCmd(serialBuf);
      serialBuf = "";
    } else {
      serialBuf += c;
      if (serialBuf.length() > 512) serialBuf = "";
    }
  }
}

void processCmd(String &raw) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  const char *t = doc["type"];
  if (!t) return;
  if (strcmp(t, "FIRE_CMD") == 0)   sendFireCmd(doc, ++txSeq);
  else if (strcmp(t, "PING_GUN") == 0) sendPing(doc["gun"] | 1);
  else if (strcmp(t, "STATUS_REQ") == 0) sendAllStatus();
  else if (strcmp(t, "WHOAMI") == 0) sendIdent(doc["nonce"] | (const char*)nullptr);
  else if (strcmp(t, "SET_KEY") == 0) setLoraKey(doc["key"] | (const char*)nullptr);
}

void sendFireCmd(JsonDocument &cmd, uint8_t seq) {
  uint8_t gun = cmd["gun"] | 1;
  int az  = cmd["az"]     | 0;
  int el  = cmd["el"]     | 0;
  int chg = cmd["charge"] | 1;
  uint8_t p[11];
  p[0]=0x10; p[1]=NODE_FDC; p[2]=gun; p[3]=seq;
  p[4]=(az>>8)&0xFF; p[5]=az&0xFF;
  p[6]=(el>>8)&0xFF; p[7]=el&0xFF;
  p[8]=chg;
  uint16_t c=crc16(p,9); p[9]=(c>>8)&0xFF; p[10]=c&0xFF;
  secureTransmit(p, 11);
  StaticJsonDocument<100> ack;
  ack["type"]="TX_OK"; ack["gun"]=gun; ack["seq"]=seq; ack["az"]=az; ack["el"]=el;
  serializeJson(ack, Serial); Serial.println();
}

void sendPing(uint8_t gun) {
  uint8_t p[6];
  p[0]=0x01; p[1]=NODE_FDC; p[2]=gun; p[3]=0;
  uint16_t c=crc16(p,4); p[4]=(c>>8)&0xFF; p[5]=c&0xFF;
  secureTransmit(p, 6);
}

void handleLoraRx() {
  if (!loraRxFlag) return;
  loraRxFlag = false;
  uint8_t env[80], buf[80];   // buf ≥ env — กันถอดรหัส (plen) ล้น stack
  int state = radio.readData(env, sizeof(env));
  if (state != RADIOLIB_ERR_NONE) { radio.startReceive(); return; }
  size_t envLen = radio.getPacketLength();
  if (envLen > sizeof(env)) envLen = sizeof(env);   // clamp: getPacketLength ได้ถึง 255 → กันถอดเกินที่รับมาจริง (buf overflow)
  float rssi = radio.getRSSI(), snr = radio.getSNR();
  // STEP B2: ถอดรหัส + ตรวจ MAC + กัน replay ก่อน — ของปลอม/ไม่เข้ารหัสถูกทิ้งเงียบๆ
  uint8_t sender = 0;
  size_t len = cryptOpen(env, envLen, buf, &sender);
  if (len < 4 || sender < 1 || sender > 4) { radio.startReceive(); return; }
  uint16_t rxCrc = ((uint16_t)buf[len-2]<<8)|buf[len-1];
  if (rxCrc != crc16(buf, len-2)) { radio.startReceive(); return; }
  uint8_t pt=buf[0], gun=buf[1];
  if (gun<1||gun>4) { radio.startReceive(); return; }
  guns[gun].rssi=rssi; guns[gun].snr=snr;
  guns[gun].lastSeen=millis(); guns[gun].active=true;

  if (pt==0x20 && len>=18) {
    float lat,lon; memcpy(&lat,&buf[3],4); memcpy(&lon,&buf[7],4);
    int az=((int16_t)buf[11]<<8)|buf[12];
    int el=((int16_t)buf[13]<<8)|buf[14];
    guns[gun].lat=lat; guns[gun].lon=lon;
    guns[gun].azMils=az; guns[gun].elMils=el; guns[gun].bat=buf[15];
    // heading (optional — v3 packet เท่านั้น len>=20)
    int hdgMil = -1;
    if (len >= 20) {
      uint16_t h = ((uint16_t)buf[16]<<8)|buf[17];
      if (h != 0xFFFF) hdgMil = (int)h;
    }
    StaticJsonDocument<256> doc;
    doc["type"]="GUN_STATUS"; doc["gun"]=gun;
    doc["lat"]=lat; doc["lon"]=lon;
    doc["az"]=az; doc["el"]=el; doc["bat"]=buf[15];
    doc["rssi"]=(int)rssi; doc["snr"]=snr;
    if (hdgMil >= 0) doc["hdg"]=hdgMil;
    serializeJson(doc,Serial); Serial.println();
  } else if (pt==0x11 && len>=6) {
    StaticJsonDocument<100> doc;
    doc["type"]="GUN_ACK"; doc["gun"]=gun;
    doc["seq"]=buf[2]; doc["ok"]=buf[3]==0x01; doc["rssi"]=(int)rssi;
    serializeJson(doc,Serial); Serial.println();
  } else if (pt==0x02) {
    StaticJsonDocument<100> doc;
    doc["type"]="PONG"; doc["gun"]=gun;
    doc["rssi"]=(int)rssi; doc["snr"]=snr;
    serializeJson(doc,Serial); Serial.println();
  }
  radio.startReceive();
}

void sendAllStatus() {
  for (int i=1;i<=4;i++) {
    if (!guns[i].active) continue;
    StaticJsonDocument<200> doc;
    doc["type"]="GUN_STATUS"; doc["gun"]=i;
    doc["lat"]=guns[i].lat; doc["lon"]=guns[i].lon;
    doc["az"]=guns[i].azMils; doc["el"]=guns[i].elMils;
    doc["bat"]=guns[i].bat; doc["rssi"]=guns[i].rssi;
    doc["snr"]=guns[i].snr;
    doc["alive"]=(millis()-guns[i].lastSeen)<30000;
    serializeJson(doc,Serial); Serial.println();
  }
}

// วาดไอคอนแบตเล็กๆ มุมขวาบน (กรอบ 18x9 + หัวขั้ว) เติมตาม %
void drawBatteryIcon(int x, int y, int pct, bool charging) {
  display.drawFrame(x, y, 18, 9);
  display.drawBox(x + 18, y + 3, 2, 3);
  int w = (constrain(pct, 0, 100) * 16) / 100;
  if (w > 0) display.drawBox(x + 1, y + 1, w, 7);
  if (charging) { display.setFont(u8g2_font_5x8_tf); display.drawStr(x - 6, y + 8, "+"); }
}

void refreshDisplay() {
  if (millis()-lastDisplay < 500) return;
  lastDisplay = millis();
  int alive=0;
  for (int i=1;i<=4;i++) if (guns[i].active&&(millis()-guns[i].lastSeen)<30000) alive++;
  DongleBat b = readDongleBat();

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  char buf[32];

  // บรรทัด 1 — พิกัด GPS + ไอคอนแบต dongle มุมขวาบน
  if (fdcGps.valid) snprintf(buf,32,"%.4f,%.4f",fdcGps.lat,fdcGps.lon);
  else snprintf(buf,32,"GPS: No Fix");
  display.drawStr(0,10,buf);

  // บรรทัด 2 — ดาว/fix
  snprintf(buf,32,"Sats:%d Fix:%d",fdcGps.sats,fdcGps.fix);
  display.drawStr(0,22,buf);

  // บรรทัด 3 — ปืนออนไลน์ + ช่อง/ความถี่ LoRa (ทุก node ในกองร้อยต้องตรงกัน)
  snprintf(buf,32,"Guns:%d/4 CH%d %.1fMHz",alive,LORA_CHANNEL,(double)LORA_FREQ);
  display.drawStr(0,34,buf);

  // บรรทัด 4 — แบตของ dongle เอง (% + แรงดัน + สถานะชาร์จ)
  if (!b.present)      snprintf(buf,32,"Pwr: USB (no batt)");
  else if (b.charging) snprintf(buf,32,"Bat:%3d%% %.2fV CHG",b.pct,b.volt);
  else                 snprintf(buf,32,"Bat:%3d%% %.2fV",b.pct,b.volt);
  display.drawStr(0,46,buf);

  // บรรทัด 5 — แบตปืนแต่ละกระบอก (-- = ออฟไลน์) ฟอนต์เล็กให้พอ 4 กระบอก
  display.setFont(u8g2_font_5x8_tf);
  char gb[40]; int pos=0;
  for (int i=1;i<=4;i++) {
    bool al = guns[i].active && (millis()-guns[i].lastSeen)<30000;
    if (al) pos += snprintf(gb+pos, sizeof(gb)-pos, "G%d:%d ", i, guns[i].bat);
    else    pos += snprintf(gb+pos, sizeof(gb)-pos, "G%d:-- ", i);
  }
  display.drawStr(0,62,gb);

  // ไอคอนแบต dongle มุมขวาบน (วาดท้ายสุด ไม่ทับข้อความ; วาบ "+" เมื่อชาร์จ/เสียบไฟ)
  drawBatteryIcon(106, 1, b.pct, b.charging || b.vbus);

  display.sendBuffer();
}
