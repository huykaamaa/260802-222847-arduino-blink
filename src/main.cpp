/*
 * Gia sach - 2 sensor doc high/low doc lap (debounce rieng), GOP lai thanh 1 trong 3 trang
 * thai sach (du sach / lay 1 cuon / lay 2 cuon). Relay1 luon ON, relay2 chay theo trang thai
 * sach (ON khi da lay sach). Bao trang thai
 * qua MQTT + OSC (3 topic/dia chi rieng, 1 cho moi trang thai).
 *
 * Nguon ghep:
 *  - ETH (W5500 SPI) + WiFi-event glue + MQTT (esp_mqtt_client) + OSC (UDP tu encode)
 *    lay nguyen tu project "can tim" (da bo phan RS485 + distance).
 *  - Sensor doc lap (pin, debounce) lay tu project "dat the", rut tu 6 xuong 2 kenh.
 *  - Diagnostic AP (phat wifi bao IP) lay tu project "dat the".
 */

#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include "html.h"
#include "ping/ping_sock.h" // esp_ping - xac minh gateway co that su tra loi (xem gatewayReachable())

// ======================================================================
// PIN CONFIG
// ======================================================================
const uint8_t sensorPins[SENSOR_NUM] = {40, 39};
// 3 cap: cap0=relay1+2 (4,5), cap1=relay3+4 (6,16), cap2=relay5+6 (15,7). Xem RELAY_PAIR_NUM
// trong globals.h.
const uint8_t relayPins[RELAY_NUM] = {5, 4, 16, 6, 7, 15};

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
// Mat khau cho diag AP (truoc day AP nay MO hoan toan). SSID da lo dia chi IP noi bo cho moi
// nguoi quanh phong quet thay, khong nen de bat ky ai cung vao thang duoc Web UI. Dung chung
// "12121212" voi cac phong khac trong cum de operator chi phai nho mot cai. Luu y WPA2 yeu cau
// toi thieu 8 ky tu - ngan hon thi softAP() se fail va mat luon duong vao nay.
static const char *DIAG_AP_PASS = "12121212";
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

char mqttTopicFull[64]        = "giasach/trangthai/1";
char mqttValueFull[32]        = "FULL";
char mqttTopicOneTaken[64]    = "giasach/trangthai/2";
char mqttValueOneTaken[32]    = "ONE_TAKEN";
char mqttTopicTwoTaken[64]    = "giasach/trangthai/3";
char mqttValueTwoTaken[32]    = "TWO_TAKEN";

// OSC
bool oscEnabled = false;
char oscIp[32]             = "192.168.99.100";
uint16_t oscPort           = 9000;
char oscAddressFull[64]        = "/composition/layers/1/clips/1/connect";
int oscValueFull               = 1;
char oscAddressOneTaken[64]    = "/composition/layers/1/clips/2/connect";
int oscValueOneTaken           = 1;
char oscAddressTwoTaken[64]    = "/composition/layers/1/clips/3/connect";
int oscValueTwoTaken           = 1;

// Admin auth
char authUser[32] = "admin";
char authPass[32] = "admin";

// ETH static-IP fallback
char ethStaticIp[16]      = "192.168.99.200";
char ethStaticGateway[16] = "192.168.99.1";
char ethStaticNetmask[16] = "255.255.255.0";
bool ethUseStaticFirst = false;

// Debounce dung chung cho ca 2 sensor
unsigned long debounceTime = 500; // ms

// Heartbeat/resync - xem giai thich trong globals.h. Mac dinh 60s.
unsigned long heartbeatInterval = 60000; // ms, 0 = tat

// Per-sensor state
bool sensorState[SENSOR_NUM]         = {false};
static bool sensorReading[SENSOR_NUM] = {false};   // raw read gan nhat, dung de bat canh
static unsigned long sensorTimer[SENSOR_NUM] = {0};
int bookState = 0; // 0 = chua xac dinh, xem BOOK_STATE_* trong globals.h

// Relay vat ly + cap nao dang bat (backup) - xem giai thich trong globals.h
bool relayOutput[RELAY_NUM] = {false};
bool relayPairEnable[RELAY_PAIR_NUM] = {true, false, false}; // mac dinh chi cap 1 (day dien goc)

// Nut Test tren web: dao trang thai CA 2 relay tam thoi. relayTestUntil CHI co y nghia khi
// relayTestPending = true (tranh bug cu: sentinel 0 = "khong test" bi hieu nham thanh so
// DUONG sau khi millis() vuot 2^31, ~24.9 ngay uptime).
static unsigned long relayTestUntil = 0;
static bool relayTestPending = false;

void triggerRelayTest() {
  relayTestUntil = millis() + RELAY_TEST_PULSE_MS;
  relayTestPending = true;
}

bool relayTestActive() {
  if (!relayTestPending) return false;
  if ((long)(relayTestUntil - millis()) > 0) return true;
  relayTestPending = false; // het xung, don co lai
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

// ======================================================================
// KIEM TRA GATEWAY CO THAT SU TRA LOI KHONG (ICMP echo)
// ======================================================================
// Ly do ton tai: ETH.config() tra ve true chi co nghia "lwIP da nhan bo IP", no KHONG thu lien
// lac voi ai ca. Nen mot IP tinh dung dinh dang nhung sai mang (vd nhap 192.168.8.4 trong khi
// LAN la 192.168.1.x) van lam eth_connected = true va bo qua han nhanh DHCP - board chot cung
// o mot IP khong ai toi duoc. Nhanh "lui ve DHCP" cu chi bat duoc truong hop khong parse duoc,
// von da bi chan tu form Save roi, nen thuc te no khong bao gio cuu duoc gi.
//
// Ping gateway la phep thu re nhat ma van chung minh duoc bo IP nay noi chuyen duoc voi mang.
// Chi can biet CO hoi dap hay khong, khong can dem - dung bool thay vi bien dem cong don
// (C++20 coi "++" tren bien volatile la deprecated, ma dem cung chang de lam gi).
static volatile bool gwPingDone = false;
static volatile bool gwPingGotReply = false;

static void onGwPingSuccess(esp_ping_handle_t hdl, void *args) { gwPingGotReply = true; }
static void onGwPingEnd(esp_ping_handle_t hdl, void *args) { gwPingDone = true; }

static bool gatewayReachable(IPAddress gw)
{
  // Cho link Ethernet len truoc da: W5500 mat 1-3s de negotiate. Ping khi day chua len thi
  // chac chan khong co hoi dap va se ket luan sai la "IP tinh hong".
  const unsigned long LINK_WAIT_MS = 5000UL;
  unsigned long t0 = millis();
  while (!ETH.linkUp() && (millis() - t0) < LINK_WAIT_MS) {
    delay(50);
  }
  if (!ETH.linkUp()) {
    // Khong co day mang thi DHCP cung chet, khong ket luan duoc gi - giu nguyen IP tinh.
    LOG("ETH: chua co link sau %lu ms - bo qua buoc ping, giu IP tinh", LINK_WAIT_MS);
    return true;
  }

  ip_addr_t target;
  memset(&target, 0, sizeof(target));
  target.type = IPADDR_TYPE_V4;
  target.u_addr.ip4.addr = (uint32_t)gw; // IPAddress va ip4_addr cung network byte order

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = target;
  cfg.count = 3;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 700;

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_ping_success = onGwPingSuccess;
  cbs.on_ping_end = onGwPingEnd;

  gwPingDone = false;
  gwPingGotReply = false;

  esp_ping_handle_t hdl = NULL;
  if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || hdl == NULL) {
    // Khong tao duoc session ping thi day la loi cua ta, khong phai loi cau hinh mang cua
    // operator - khong lay do lam co de vut bo IP tinh.
    LOG("ETH: khong tao duoc phien ping - bo qua buoc kiem tra, giu IP tinh");
    return true;
  }

  esp_ping_start(hdl);
  const unsigned long PING_TOTAL_MS = 4000UL;
  t0 = millis();
  while (!gwPingDone && (millis() - t0) < PING_TOTAL_MS) {
    delay(20);
  }
  esp_ping_stop(hdl);
  esp_ping_delete_session(hdl);

  return gwPingGotReply;
}

// Phat 1 AP co SSID chua IP hien tai cua ETH, de xem IP qua wifi tren dien thoai thay vi phai
// mo Serial. Co mat khau DIAG_AP_PASS. Tu tat sau DIAG_AP_DURATION_MS (xu ly trong loop()).
static void startDiagAp(bool isFallback) {
  String ssid = (isFallback ? "GIASACH-STATIC-" : "GIASACH-DHCP-") + ETH.localIP().toString();
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(ssid.c_str(), DIAG_AP_PASS)) {
    diagApActive = true;
    diagApStartMs = millis();
    LOG("Diag AP: broadcasting %s", ssid.c_str());
  } else {
    LOG("Diag AP: softAP() failed");
  }
}

// ======================================================================
// SENSOR + RELAY LOOP - debounce rieng tung sensor, roi GOP thanh 1 trong 3 trang thai sach
// ======================================================================
static void checkSensors() {
  for (int i = 0; i < SENSOR_NUM; i++) {
    bool active = (digitalRead(sensorPins[i]) == SENSOR_ACTIVE);

    // debounce (dung chung 1 gia tri debounceTime cho ca 2 sensor, moi kenh co timer rieng)
    if (active != sensorReading[i]) {
      sensorReading[i] = active;
      sensorTimer[i] = millis();
    }
    if (millis() - sensorTimer[i] >= debounceTime) {
      sensorState[i] = sensorReading[i];
    }
  }

  // Ca 2 sensor kich (2 cuon sach dang o tren ke) = du sach. Dung 1 trong 2 kich = da lay 1
  // cuon. Khong sensor nao kich = da lay ca 2 cuon.
  bool s1 = sensorState[0];
  bool s2 = sensorState[1];
  int newState;
  if (s1 && s2) newState = BOOK_STATE_FULL;
  else if (s1 != s2) newState = BOOK_STATE_ONE_TAKEN;
  else newState = BOOK_STATE_TWO_TAKEN;

  // relay1 LUON ON (nguon/den nen, khong phu thuoc trang thai sach); relay2 on o ca 2 trang
  // thai "da lay sach" (1 hoac 2 cuon) va tiep tuc ON o trang thai lay 2 cuon, khong tu tat.
  bool relay1Logic = true;
  bool relay2Logic = (newState != BOOK_STATE_FULL);

  // Nut Test: dao NGUOC tin hieu relay1/relay2 tam thoi de kiem tra day dien, bat ke trang
  // thai sach that. Khong dung vao bookState/relay*Logic ngoai xung test - het xung thi vong
  // loop() ke tiep tinh lai tu bookState nhu binh thuong, tu dong "resync" ve dung trang thai,
  // khong can goi gi them.
  if (relayTestActive()) {
    relay1Logic = !relay1Logic;
    relay2Logic = !relay2Logic;
  }

  // Phan tin hieu relay1/relay2 ra CA cac cap relay vat ly dang duoc tick (backup) - cap
  // khong tick thi luon OFF du logic la gi.
  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    bool on = relayPairEnable[p];
    relayOutput[2 * p]     = on && relay1Logic;
    relayOutput[2 * p + 1] = on && relay2Logic;
    digitalWrite(relayPins[2 * p],     relayOutput[2 * p]     ? RELAY_ON : RELAY_OFF);
    digitalWrite(relayPins[2 * p + 1], relayOutput[2 * p + 1] ? RELAY_ON : RELAY_OFF);
  }

  if (newState != bookState) {
    bookState = newState;
    triggerBookState(bookState);
  }
}

// Gui lai trang thai sach HIEN TAI qua MQTT/OSC, dung nguyen topic/dia chi/gia tri nhu binh
// thuong - khong phai message rieng biet, chi la "nhac lai" cue gan nhat. Bu lai truong hop 1
// lan doi trang thai bi rot mang. Dung cho heartbeat dinh ky va cho buoc ket thuc chuoi Test
// trong web.cpp.
void resyncBookState() {
  if (bookState == 0) return; // chua co lan check nao xong, chua co gi de nhac lai
  triggerBookState(bookState);
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
  }
  for (int i = 0; i < RELAY_NUM; i++) {
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

  // Uu tien IP tinh: ap dung ngay, bo qua hoan toan cho DHCP (boot nhanh hon, dung khi mang
  // khong co DHCP server hoac muon IP co dinh chac chan). Ap xong PHAI ping gateway de xac
  // minh bo IP nay thuc su noi chuyen duoc voi mang - xem gatewayReachable() ve ly do.
  if (ethUseStaticFirst) {
    IPAddress ip, gw, mask;
    if (ip.fromString(ethStaticIp) && gw.fromString(ethStaticGateway) && mask.fromString(ethStaticNetmask)) {
      // Truyen gateway lam DNS1: ETH.config() 3 tham so de DNS trong, nen neu MQTT broker
      // duoc nhap bang hostname thay vi IP thi se khong resolve duoc o nhanh IP tinh.
      if (ETH.config(ip, gw, mask, gw)) {
        if (gatewayReachable(gw)) {
          eth_connected = true;
          usedFallback = true;
          LOG("ETH: dung IP tinh ngay tu dau (uu tien) - %s (gw %s, mask %s), gateway tra loi ping", ethStaticIp, ethStaticGateway, ethStaticNetmask);
        } else {
          // Gateway khong tra loi -> nhieu kha nang IP tinh sai mang. Tra netif ve DHCP
          // (local_ip = 0 lam esp_netif khoi dong lai DHCP client, xem NetworkInterface::
          // config) roi de nhanh cho DHCP ben duoi chay nhu binh thuong.
          //
          // Neu router chan ICMP thi day la canh bao gia: gia phai tra la ~10s cho DHCP, va
          // neu DHCP cung khong len thi nhanh static fallback ben duoi VAN ap lai dung bo IP
          // tinh nay. Nen truong hop xau nhat chi la boot cham hon, khong mat board.
          LOG("ETH: gateway %s KHONG tra loi ping - IP tinh %s nhieu kha nang sai mang, chuyen sang thu DHCP", ethStaticGateway, ethStaticIp);
          ETH.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
          eth_connected = false;
        }
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
  server.on("/test_iot", HTTP_POST, handleTestIot);
  server.on("/test_relay", HTTP_POST, handleTestRelay);
  server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);
  server.on("/reboot", HTTP_POST, handleReboot);
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
    resyncBookState();
  }
}
