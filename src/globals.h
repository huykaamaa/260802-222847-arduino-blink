#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <ETH.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mqtt_client.h>

#define LOG(fmt, ...) do { Serial.printf(fmt "\r\n", ##__VA_ARGS__); } while (0)

// --- NVS / Preferences key-length guard --------------------------------------------------
// ESP32 NVS caps key names at 15 usable chars + NUL. Wrapping every NVS key literal in
// NVS_KEY() fails the BUILD instead of silently losing data if a key is ever renamed past
// that limit.
template <size_t N>
constexpr const char* NVS_KEY(const char (&s)[N]) {
  static_assert(N <= 16, "NVS key too long: max 15 chars + NUL");
  return s;
}

// ======================================================================
// PIN CONFIG
// ======================================================================

// ETH (W5500 over SPI)
#define ETH_CS   10
#define ETH_MOSI 11
#define ETH_MISO 13
#define ETH_SCK  12
#define ETH_INT  -1
#define ETH_RST  -1

// Chi con 2 sensor (vi tri 1+2), logic GOP: xem checkSensors() trong main.cpp.
#define SENSOR_NUM 2
extern const uint8_t sensorPins[SENSOR_NUM];

// Van con du 6 relay vat ly (day dien san co tu truoc) - dung 3 CAP relay de BACKUP: cap nao
// duoc tick (relayPairEnable) thi nhan cung tin hieu relay1 (luon ON) / relay2 (theo sach), cap nao
// khong tick thi luon OFF. Tick nhieu cap cung luc = chay song song du phong; hong 1 cap thi bo
// tick cap do, tick cap con lai, khong can nap lai firmware.
#define RELAY_NUM 6
#define RELAY_PAIR_NUM (RELAY_NUM / 2)
extern const uint8_t relayPins[RELAY_NUM];
extern bool relayPairEnable[RELAY_PAIR_NUM];

#define SENSOR_ACTIVE LOW
#define RELAY_ON  HIGH
#define RELAY_OFF LOW
#define RELAY_TEST_PULSE_MS 2000UL

// Trang thai sach: 1 = du sach (ca 2 sensor kich), 2 = da lay 1 cuon (dung 1 trong 2 sensor
// kich), 3 = da lay ca 2 cuon (khong sensor nao kich).
#define BOOK_STATE_FULL      1
#define BOOK_STATE_ONE_TAKEN 2
#define BOOK_STATE_TWO_TAKEN 3

// Local UDP port de mo socket OSC (chi gui di, khong nghe). Tach ra hang so de khong bi
// nham voi oscPort (port DICH, cau hinh duoc tren web).
#define OSC_LOCAL_PORT 9000

// ======================================================================
// STATE / CONFIG - dinh nghia trong main.cpp
// ======================================================================
extern SPIClass spi;
extern WebServer server;
extern Preferences prefs;
extern WiFiUDP oscUdp;
extern bool eth_connected;

extern bool diagApActive;
extern unsigned long diagApStartMs;

// MQTT
extern esp_mqtt_client_handle_t mqtt;
extern bool mqttConnected;
extern bool mqttEnabled;
extern char mqttServer[32];
extern uint16_t mqttPort;
extern char mqttUser[32];
extern char mqttPass[32];

// Moi trang thai (FULL/ONE_TAKEN/TWO_TAKEN) co topic + payload rieng, ban 1 lan khi VUA
// CHUYEN vao trang thai do (edge-triggered, giong OSC ben duoi).
extern char mqttTopicFull[64];
extern char mqttValueFull[32];
extern char mqttTopicOneTaken[64];
extern char mqttValueOneTaken[32];
extern char mqttTopicTwoTaken[64];
extern char mqttValueTwoTaken[32];

// OSC - 3 trang thai = 3 message OSC hoan toan tach biet (dia chi rieng + int rieng moi cai).
extern bool oscEnabled;
extern char oscIp[32];
extern uint16_t oscPort;
extern char oscAddressFull[64];
extern int oscValueFull;
extern char oscAddressOneTaken[64];
extern int oscValueOneTaken;
extern char oscAddressTwoTaken[64];
extern int oscValueTwoTaken;

// Admin auth (Basic Auth tren cac endpoint thay doi trang thai)
extern char authUser[32];
extern char authPass[32];

// ETH static-IP: mac dinh la fallback (chi dung khi DHCP khong len duoc). Neu
// ethUseStaticFirst=true thi dung IP tinh ngay tu dau, bo qua cho DHCP (10s) hoan toan;
// van tu dong lui ve DHCP neu IP tinh nhap sai/khong parse duoc.
extern char ethStaticIp[16];
extern char ethStaticGateway[16];
extern char ethStaticNetmask[16];
extern bool ethUseStaticFirst;

// Debounce dung chung cho ca 2 sensor
extern unsigned long debounceTime;

// Heartbeat/resync: dinh ky gui lai trang thai sach hien tai qua MQTT/OSC (khong doi
// topic/dia chi, chi gui lai gia tri hien tai) - de bu lai neu message tai thoi diem doi
// trang thai bi rot mang (MQTT QoS0/UDP OSC deu khong dam bao gui toi noi). 0 = tat heartbeat.
extern unsigned long heartbeatInterval;

// Per-sensor state (debounce), tach biet voi relay dau ra (relay la KET QUA cua logic gop
// ca 2 sensor, khong con 1:1 voi tung sensor nua - xem checkSensors() trong main.cpp).
extern bool sensorState[SENSOR_NUM];   // trang thai da debounce: true = co sach o vi tri do
extern bool relayOutput[RELAY_NUM];    // trang thai THUC TE tung relay vat ly (0..5), hien len web
extern int bookState;                  // BOOK_STATE_FULL / _ONE_TAKEN / _TWO_TAKEN, 0 = chua xac dinh (truoc lan check dau)

// Nut Test tren web: dao trang thai CA 2 relay tam thoi (~2s) de kiem tra day dien, bat ke
// trang thai sach thuc te. checkSensors() tu dong tra ve dung trang thai (tinh theo bookState)
// ngay khi xung Test ket thuc - khong can goi resync rieng, vi relay duoc tinh lai moi vong
// loop() thay vi edge-triggered nhu MQTT/OSC. Dinh nghia trong main.cpp.
void triggerRelayTest();
bool relayTestActive();

// Gui lai trang thai sach HIEN TAI qua MQTT/OSC, dung nguyen topic/dia chi/gia tri binh
// thuong - chi la "nhac lai" cue gan nhat, khong phai message rieng. Dung cho heartbeat dinh
// ky va cho buoc ket thuc chuoi Test. Dinh nghia trong main.cpp.
void resyncBookState();
