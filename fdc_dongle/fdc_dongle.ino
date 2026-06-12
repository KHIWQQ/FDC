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

// LoRa Config
#define LORA_FREQ     868.0
#define LORA_BW        62.5
#define LORA_SF        12
#define LORA_CR         5
#define LORA_SYNC    0xAB
#define LORA_POWER     22
#define LORA_PREAMBLE   8
#define NODE_FDC      0x01

XPowersAXP2101 pmu;
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

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

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
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("{\"type\":\"ERROR\",\"msg\":\"LoRa failed %d\"}\n", state);
    while (true) delay(1000);
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
  display.drawStr(0, 14, "FDC Dongle v2");
  display.drawStr(0, 28, "LoRa OK 868MHz");
  display.drawStr(0, 42, "GPS Acquiring...");
  display.sendBuffer();

  Serial.println("{\"type\":\"INFO\",\"msg\":\"FDC Dongle ready\"}");
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
  radio.transmit(p, 11);
  radio.startReceive();
  StaticJsonDocument<100> ack;
  ack["type"]="TX_OK"; ack["gun"]=gun; ack["seq"]=seq; ack["az"]=az; ack["el"]=el;
  serializeJson(ack, Serial); Serial.println();
}

void sendPing(uint8_t gun) {
  uint8_t p[6];
  p[0]=0x01; p[1]=NODE_FDC; p[2]=gun; p[3]=0;
  uint16_t c=crc16(p,4); p[4]=(c>>8)&0xFF; p[5]=c&0xFF;
  radio.transmit(p, 6); radio.startReceive();
}

void handleLoraRx() {
  if (!loraRxFlag) return;
  loraRxFlag = false;
  uint8_t buf[64];
  int state = radio.readData(buf, sizeof(buf));
  if (state != RADIOLIB_ERR_NONE) { radio.startReceive(); return; }
  size_t len = radio.getPacketLength();
  float rssi = radio.getRSSI(), snr = radio.getSNR();
  if (len < 4) { radio.startReceive(); return; }
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

void refreshDisplay() {
  if (millis()-lastDisplay < 500) return;
  lastDisplay = millis();
  int alive=0;
  for (int i=1;i<=4;i++) if (guns[i].active&&(millis()-guns[i].lastSeen)<30000) alive++;
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  char buf[32];
  if (fdcGps.valid) snprintf(buf,32,"%.4f,%.4f",fdcGps.lat,fdcGps.lon);
  else snprintf(buf,32,"GPS: No Fix");
  display.drawStr(0,10,buf);
  snprintf(buf,32,"Sats:%d Fix:%d",fdcGps.sats,fdcGps.fix);
  display.drawStr(0,22,buf);
  snprintf(buf,32,"Guns: %d/4 online",alive);
  display.drawStr(0,34,buf);
  display.sendBuffer();
}
