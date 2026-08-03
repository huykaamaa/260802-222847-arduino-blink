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

#define SENSOR_NUM 6
extern const uint8_t sensorPins[SENSOR_NUM];
extern const uint8_t relayPins[SENSOR_NUM];

#define SENSOR_ACTIVE LOW
#define RELAY_ON  HIGH
#define RELAY_OFF LOW
#define RELAY_TEST_PULSE_MS 2000UL

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
extern char mqttTopic[64];        // moi vi tri se publish vao "<mqttTopic>/<1..6>"
extern char mqttFullValue[32];
extern char mqttMissingValue[32];

// OSC - CO va TRONG la 2 message OSC hoan toan tach biet (dia chi rieng + int rieng).
// "{id}" trong 2 dia chi duoc thay bang vi tri (1..6) truoc khi gui.
extern bool oscEnabled;
extern char oscIp[32];
extern uint16_t oscPort;
extern char oscAddressFull[64];
extern char oscAddressMissing[64];
extern int oscValueFull;
extern int oscValueMissing;

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

// Debounce dung chung cho ca 6 vi tri
extern unsigned long debounceTime;

// Heartbeat/resync: dinh ky gui lai trang thai hien tai cua ca 6 vi tri qua MQTT/OSC
// (khong doi topic/dia chi, chi gui lai gia tri hien tai) - de bu lai neu 1 message tai
// thoi diem doi trang thai bi rot mang (MQTT QoS0/UDP OSC deu khong dam bao gui toi noi).
// 0 = tat heartbeat.
extern unsigned long heartbeatInterval;

// Per-sensor state
extern bool sensorEnable[SENSOR_NUM];
extern bool relayState[SENSOR_NUM];   // trang thai da debounce cua sensor
extern bool relayOutput[SENSOR_NUM];  // trang thai relay THUC TE (da gate enable + test), hien len web
extern unsigned long relayTestUntil[SENSOR_NUM]; // nut Test tren web: ep relay ON tam thoi

// dinh nghia trong main.cpp, goi tu web.cpp (nut Test tren web)
void triggerRelayTest(int id);
