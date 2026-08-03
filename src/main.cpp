/*
 * Gia sach - moi sensor (vi tri) doc high/low doc lap, tu kich relay rieng,
 * dong thoi bao trang thai qua MQTT + OSC rieng tung vi tri (khong co trang thai tong).
 *
 * Nguon ghep:
 *  - ETH (W5500 SPI) + WiFi-event glue + MQTT (esp_mqtt_client) + OSC (UDP tu encode)
 *    lay nguyen tu project "can tim" (da bo phan RS485 + distance).
 *  - Sensor/relay 6 kenh doc lap (pin, debounce, follow-relay) lay tu project "dat the".
 *  - Diagnostic AP (phat wifi bao IP) lay tu project "dat the".
 */

#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include "html.h"

// ======================================================================
// PIN CONFIG
// ======================================================================
const uint8_t sensorPins[SENSOR_NUM] = {1, 2, 42, 41, 40, 39};
const uint8_t relayPins[SENSOR_NUM]  = {4, 5, 6, 16, 15, 7};

// ======================================================================
// STATE / CONFIG
// ======================================================================
SPIClass spi(FSPI);
WebServer server(80);
Preferences prefs;
WiFiUDP oscUdp;
bool eth_connected = false;

// Diagnostic AP - phat wifi de doc IP ETH tren dien thoai, tu tat sau 5 phut
static const unsigned long DIAG_AP_DURATION_MS = 5UL * 60UL * 1000UL;
bool diagApActive = false;
unsigned long diagApStartMs = 0;

// MQTT
esp_mqtt_client_handle_t mqtt = nullptr;
bool mqttConnected = false;
bool mqttEnabled = true;
char mqttServer[32]       = "192.168.99.225";
uint16_t mqttPort         = 1883;
char mqttUser[32]         = "huykaamaa";
// Mac dinh RONG - khong hardcode mat khau that vao source (source nam trong git). Nhap 1 lan
// qua Web UI, sau do no nam trong NVS va song qua cac lan nap firmware moi.
char mqttPass[32]         = "";
char mqttTopic[64]        = "giasach/vitri"; // moi vi tri se publish vao "<mqttTopic>/<1..6>"
char mqttFullValue[32]    = "FULL";
char mqttMissingValue[32] = "MISSING";

// OSC
bool oscEnabled = false;
char oscIp[32]             = "192.168.99.100";
uint16_t oscPort           = 9000;
char oscAddressFull[64]    = "/composition/layers/1/clips/{id}/connect";
char oscAddressMissing[64] = "/composition/layers/1/clips/{id}/disconnect";
int oscValueFull     = 1;
int oscValueMissing  = 1;

// Admin auth
char authUser[32] = "admin";
char authPass[32] = "admin";

// ETH static-IP fallback
char ethStaticIp[16]      = "192.168.99.200";
char ethStaticGateway[16] = "192.168.99.1";
char ethStaticNetmask[16] = "255.255.255.0";
bool ethUseStaticFirst = false;

// Debounce dung chung cho ca 6 vi tri
unsigned long debounceTime = 500; // ms

// Heartbeat/resync - xem giai thich trong globals.h. Mac dinh 60s.
unsigned long heartbeatInterval = 60000; // ms, 0 = tat

// Per-sensor state
bool sensorEnable[SENSOR_NUM]        = {true, true, true, true, true, true};
bool relayState[SENSOR_NUM]          = {false};
bool relayOutput[SENSOR_NUM]         = {false};
static bool relayReading[SENSOR_NUM] = {false};   // raw read gan nhat, dung de bat canh
static unsigned long relayTimer[SENSOR_NUM] = {0};
static bool lastSentState[SENSOR_NUM] = {false};  // trang thai (state && enable) da bao MQTT/OSC lan gan nhat

// Nut Test tren web: ep relay ON tam thoi. relayTestUntil CHI co y nghia khi relayTestPending
// = true. Truoc day chi co relayTestUntil voi sentinel 0 nghia la "khong test", nhung phep tru
// chong tran (0 - millis()) lai ra so DUONG khi millis() > 2^31 (~24.9 ngay uptime) - moi relay
// chua tung bam Test se bi ep ON suot 24.9 ngay ke tiep. Co pending tach bach "co moc hop le
// khong" khoi "moc do da qua chua", nen khong con phu thuoc vao gia tri sentinel nao ca.
static unsigned long relayTestUntil[SENSOR_NUM] = {0};
static bool relayTestPending[SENSOR_NUM]        = {false};

bool relayTestActive(int id) {
  if (id < 0 || id >= SENSOR_NUM) return false;
  if (!relayTestPending[id]) return false;
  if ((long)(relayTestUntil[id] - millis()) > 0) return true;
  relayTestPending[id] = false;  // het xung, don co lai
  return false;
}

// ======================================================================
// ETH EVENT
// ======================================================================
static void WiFiEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      LOG("ETH Started");
      ETH.setHostname("ESP32-GiaSach");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      LOG("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      LOG("ETH Got IP: %s", ETH.localIP().toString().c_str());
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      LOG("ETH Disconnected");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      LOG("ETH Stopped");
      eth_connected = false;
      break;
    default:
      break;
  }
}

// Phat 1 AP mo (khong mat khau) co SSID chua IP hien tai cua ETH, de xem IP qua wifi tren
// dien thoai thay vi phai mo Serial. Tu tat sau DIAG_AP_DURATION_MS (xu ly trong loop()).
static void startDiagAp(bool isFallback) {
  String ssid = (isFallback ? "GIASACH-STATIC-" : "GIASACH-DHCP-") + ETH.localIP().toString();
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(ssid.c_str())) {
    diagApActive = true;
    diagApStartMs = millis();
    LOG("Diag AP: broadcasting %s", ssid.c_str());
  } else {
    LOG("Diag AP: softAP() failed");
  }
}

void triggerRelayTest(int id) {
  if (id < 0 || id >= SENSOR_NUM) return;
  relayTestUntil[id] = millis() + RELAY_TEST_PULSE_MS;
  relayTestPending[id] = true;
}

// ======================================================================
// SENSOR + RELAY LOOP - moi vi tri doc lap hoan toan, khong co trang thai tong
// ======================================================================
static void checkSensors() {
  for (int i = 0; i < SENSOR_NUM; i++) {
    bool active = (digitalRead(sensorPins[i]) == SENSOR_ACTIVE);

    // debounce (dung chung 1 gia tri debounceTime cho ca 6, moi kenh co timer rieng)
    if (active != relayReading[i]) {
      relayReading[i] = active;
      relayTimer[i] = millis();
    }
    if (millis() - relayTimer[i] >= debounceTime) {
      relayState[i] = relayReading[i];
    }

    // chi vi tri duoc tick (sensorEnable) moi thuc su kick relay; nut Test tren web
    // van ep ON tam thoi (2s) bat ke co tick hay khong, de kiem tra day dien.
    bool testActive = relayTestActive(i);
    relayOutput[i] = (relayState[i] && sensorEnable[i]) || testActive;
    digitalWrite(relayPins[i], relayOutput[i] ? RELAY_ON : RELAY_OFF);

    // Bao MQTT/OSC khi trang thai THUC TE (co tinh enable, KHONG tinh test-pulse) doi -
    // tuc la ca khi sensor doi trang thai LAN khi checkbox Enable vua duoc tick/bo tick tren
    // web, de nguoi nhan khong bi ket o cue cu sau khi bat/tat 1 vi tri.
    bool effectiveState = relayState[i] && sensorEnable[i];
    if (effectiveState != lastSentState[i]) {
      lastSentState[i] = effectiveState;
      triggerSensor(i, effectiveState);
    }
  }
}

// Gui lai trang thai HIEN TAI (khong tinh test-pulse) cua ca 6 vi tri qua MQTT/OSC, dung
// nguyen topic/dia chi/gia tri nhu binh thuong - khong phai message rieng biet, chi la
// "nhac lai" cue gan nhat. Bu lai truong hop 1 lan doi trang thai bi rot mang (F31-style,
// xem docs/todo tuong ung ben project can tim). Dung cho heartbeat dinh ky va cho buoc
// ket thuc chuoi Test trong web.cpp.
void resyncAllPositions() {
  for (int i = 0; i < SENSOR_NUM; i++) {
    triggerSensor(i, lastSentState[i]);
  }
}

// ======================================================================
// SETUP / LOOP
// ======================================================================
void setup() {
  Serial.begin(115200);
  unsigned long serialStart = millis();
  while (!Serial && (millis() - serialStart) < 2000) delay(10);

  for (int i = 0; i < SENSOR_NUM; i++) {
    pinMode(sensorPins[i], INPUT_PULLUP);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
  }

  loadConfig();

  WiFi.onEvent(WiFiEvent);
  spi.begin(ETH_SCK, ETH_MISO, ETH_MOSI, ETH_CS);
  if (!ETH.begin(ETH_PHY_W5500, 1, ETH_CS, ETH_INT, ETH_RST, spi)) {
    LOG("ETH Failed");
  }

  bool usedFallback = false;

  // Uu tien IP tinh: ap dung ngay, bo qua hoan toan cho DHCP (boot nhanh hon, dung khi
  // mang khong co DHCP server hoac muon IP co dinh chac chan). Neu IP tinh nhap sai/khong
  // parse duoc thi tu dong lui ve nhanh DHCP-roi-fallback ben duoi, khong bi ket cung.
  if (ethUseStaticFirst) {
    IPAddress ip, gw, mask;
    if (ip.fromString(ethStaticIp) && gw.fromString(ethStaticGateway) && mask.fromString(ethStaticNetmask)) {
      // Truyen gateway lam DNS1: ETH.config() 3 tham so de DNS trong, nen neu MQTT broker
      // duoc nhap bang hostname thay vi IP thi se khong resolve duoc o nhanh IP tinh.
      if (ETH.config(ip, gw, mask, gw)) {
        eth_connected = true;
        usedFallback = true;
        LOG("ETH: dung IP tinh ngay tu dau (uu tien) - %s (gw %s, mask %s)", ethStaticIp, ethStaticGateway, ethStaticNetmask);
      } else {
        LOG("ETH: ETH.config() (uu tien IP tinh) that bai - chuyen sang thu DHCP");
      }
    } else {
      LOG("ETH: IP tinh (uu tien) khong parse duoc - chuyen sang thu DHCP");
    }
  }

  if (!eth_connected) {
    const unsigned long ETH_WAIT_MS = 10000UL;
    unsigned long ethStart = millis();
    while (!eth_connected && (millis() - ethStart) < ETH_WAIT_MS) {
      delay(100);
    }

    if (!eth_connected) {
      LOG("ETH khong len IP sau %lu ms, ap dung static fallback de Web UI van truy cap duoc", ETH_WAIT_MS);
      IPAddress fallbackIp, fallbackGw, fallbackMask;
      if (fallbackIp.fromString(ethStaticIp) && fallbackGw.fromString(ethStaticGateway) && fallbackMask.fromString(ethStaticNetmask)) {
        if (ETH.config(fallbackIp, fallbackGw, fallbackMask, fallbackGw)) { // gw lam DNS1, xem tren
          eth_connected = true;
          usedFallback = true;
          LOG("ETH: static fallback IP %s (gw %s, mask %s)", ethStaticIp, ethStaticGateway, ethStaticNetmask);
        } else {
          LOG("ETH: static fallback ETH.config() that bai");
        }
      } else {
        LOG("ETH: static fallback IP/gw/mask khong parse duoc - kiem tra cau hinh Mang");
      }
    }
  }

  if (eth_connected) {
    startDiagAp(usedFallback);
  } else {
    LOG("ETH khong co IP - khong the phat diag AP, chi con Serial de debug");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/test_mqtt", HTTP_POST, handleTestMQTT);
  server.on("/test_osc", HTTP_POST, handleTestOSC);
  server.on("/test_relay", HTTP_POST, handleTestRelay);
  server.begin();
  oscUdp.begin(OSC_LOCAL_PORT);

  mqttInit();
  LOG("HTTP Server Started");
}

void loop() {
  if (eth_connected) {
    server.handleClient();
  }

  if (diagApActive && (millis() - diagApStartMs) >= DIAG_AP_DURATION_MS) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    diagApActive = false;
    LOG("Diag AP: turned off");
  }

  updateTestSequence();
  checkSensors();

  // Khoi tao bang millis() (chay dung 1 lan) chu khong phai 0: uptime luc vao loop() da ~12s
  // (cho Serial + cho DHCP), nen voi chu ky heartbeat ngan (min 5s) moc 0 se lam nhip dau ban
  // ngay o vong loop dau tien. Dat sau checkSensors() de lan gui dau tien chac chan da co
  // trang thai sensor that, khong phai gia tri khoi tao toan FALSE (=TRONG).
  static unsigned long lastHeartbeatMs = millis();
  if (heartbeatInterval > 0 && (millis() - lastHeartbeatMs) >= heartbeatInterval) {
    lastHeartbeatMs = millis();
    resyncAllPositions();
  }
}
