/*
 * NODE PHU - gia sach.
 *
 * Nhiem vu duy nhat: doc 2 sensor TCRT5000 (debounce tai cho), gop thanh 1 trong 3 trang
 * thai sach roi ban ve node chinh qua ESP-NOW. Relay / MQTT / OSC / Web UI van nam het o
 * node chinh, board nay khong biet gi ve chung.
 *
 * Vi sao ESP-NOW chu khong phai MQTT qua WiFi: khong phu thuoc router/AP/broker, tre ~2-5 ms,
 * boot xong la chay ngay (khong cho DHCP). Doi lai phai biet truoc MAC node chinh - xem
 * MAIN_NODE_MAC trong node_config.h.
 *
 * Chieu nguoc lai (node chinh -> node phu) chi co 2 lenh: PING (do link) va REBOOT (reset
 * tu xa, khoi phai treo thang len bat lai nguon). Lenh duoc bao ve 2 lop: ma hoa ESP-NOW
 * (PMK/LMK) + token nonce dung mot lan - xem shared/giasach_espnow.h.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <esp_idf_version.h>

#include "giasach_espnow.h"
#include "node_config.h"

#define LOG(fmt, ...) do { Serial.printf(fmt "\r\n", ##__VA_ARGS__); } while (0)

// ======================================================================
// STATE
// ======================================================================
static bool sensorState[NODE_SENSOR_NUM]   = {false}; // da debounce: true = co sach
static bool sensorReading[NODE_SENSOR_NUM] = {false}; // raw doc gan nhat
static unsigned long sensorTimer[NODE_SENSOR_NUM] = {0};
static int bookState = GIASACH_STATE_UNKNOWN;

static uint16_t txSeq = 0;
static uint32_t currentNonce = 0;   // thu thach hien tai cho lenh tu node chinh
static bool bootFlagPending = true; // goi dau tien sau boot deo co GIASACH_FLAG_BOOT

// Chum goi khi doi trang thai (xem NODE_BURST_COUNT trong node_config.h)
static uint8_t burstLeft = 0;
static unsigned long nextBurstMs = 0;
static unsigned long lastHeartbeatMs = 0;

// Co set tu callback ESP-NOW, XU LY TRONG loop(). Callback chay trong task WiFi - khong goi
// esp_now_send() hay esp_restart() thang trong do.
static volatile bool pingRequested = false;
static volatile bool rebootRequested = false;

// Theo doi suc khoe duong truyen
static unsigned long lastSendOkMs = 0;
static bool everSentOk = false;     // chua tung gui duoc thi KHONG tu reboot (xem loop())
static uint32_t sendFailStreak = 0;

// ======================================================================
// ESP-NOW CALLBACKS
// ======================================================================
// Chu ky callback gui doi theo IDF 5.4 (Arduino core 3.2+): mac_addr -> wifi_tx_info_t.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void onDataSent(const wifi_tx_info_t* /*info*/, esp_now_send_status_t status) {
#else
static void onDataSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) {
    lastSendOkMs = millis();
    if (!everSentOk) {
      everSentOk = true;
      LOG("ESP-NOW: goi dau tien toi node chinh THANH CONG");
    }
    if (sendFailStreak > 0) {
      LOG("ESP-NOW: noi lai duoc sau %lu goi that bai", (unsigned long)sendFailStreak);
      sendFailStreak = 0;
    }
  } else {
    sendFailStreak++;
    // Chi log thua thot, khong moi goi rot deu in - het 1 phut mat song la Serial ngap.
    if (sendFailStreak == 1 || (sendFailStreak % 100) == 0) {
      LOG("ESP-NOW: send FAIL (streak %lu) - kiem tra MAC node chinh / kenh / khoa",
          (unsigned long)sendFailStreak);
    }
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* src = info->src_addr;
#else
static void onDataRecv(const uint8_t* src, const uint8_t* data, int len) {
#endif
  // Chi nghe node chinh. Goi tu MAC khac vut thang, khong log de khong bi spam.
  if (memcmp(src, MAIN_NODE_MAC, 6) != 0) return;

  if (len != (int)sizeof(GiaSachCmdMsg)) {
    LOG("CMD: sai kich thuoc goi (%d, can %u)", len, (unsigned)sizeof(GiaSachCmdMsg));
    return;
  }

  GiaSachCmdMsg msg;
  memcpy(&msg, data, sizeof(msg));

  if (msg.magic != GIASACH_MAGIC) return; // he khac, khong phai loi
  if (msg.version != GIASACH_PROTO_VERSION) {
    LOG("CMD: lech version giao thuc (nhan %u, minh %u) - nap lai firmware ca 2 board",
        (unsigned)msg.version, (unsigned)GIASACH_PROTO_VERSION);
    return;
  }
  if (msg.type != GIASACH_MSG_CMD) return;

  // Nonce phai la cai node nay VUA phat ra -> lenh cu ghi lai khong dung lai duoc.
  if (msg.nonce != currentNonce) {
    LOG("CMD: nonce khong khop (lenh cu hoac bi phat lai) - bo qua");
    return;
  }
  if (msg.token != giaSachCmdToken(msg.nonce, msg.cmd, GIASACH_CMD_SECRET)) {
    LOG("CMD: token sai - bo qua (secret hai ben co the dang lech)");
    return;
  }

  // Lenh hop le: doi nonce NGAY, ke ca lenh khong nhan ra o duoi. Mot nonce = mot lenh.
  currentNonce = esp_random();

  switch (msg.cmd) {
    case GIASACH_CMD_PING:
      pingRequested = true;
      break;
    case GIASACH_CMD_REBOOT:
      LOG("CMD: nhan lenh REBOOT tu node chinh");
      rebootRequested = true;
      break;
    default:
      LOG("CMD: lenh la 0x%02X - bo qua", (unsigned)msg.cmd);
      break;
  }
}

// ======================================================================
// GUI TRANG THAI
// ======================================================================
static void sendState(uint8_t extraFlags) {
  GiaSachStateMsg msg = {};
  msg.magic     = GIASACH_MAGIC;
  msg.version   = GIASACH_PROTO_VERSION;
  msg.type      = GIASACH_MSG_STATE;
  msg.seq       = ++txSeq;
  msg.sensor1   = sensorState[0] ? 1 : 0;
  msg.sensor2   = sensorState[1] ? 1 : 0;
  msg.bookState = (uint8_t)bookState;
  msg.flags     = extraFlags;
  msg.uptimeMs  = (uint32_t)millis();
  msg.nonce     = currentNonce;

  if (bootFlagPending) {
    msg.flags |= GIASACH_FLAG_BOOT;
    bootFlagPending = false;
  }
  if (millis() < NODE_SETTLE_MS) {
    msg.flags |= GIASACH_FLAG_SETTLING;
  }

  esp_err_t err = esp_now_send(MAIN_NODE_MAC, (const uint8_t*)&msg, sizeof(msg));
  if (err != ESP_OK) {
    LOG("ESP-NOW: esp_now_send() loi %d (%s)", (int)err, esp_err_to_name(err));
  }
  lastHeartbeatMs = millis();
}

// Trang thai doi -> ban mot chum de bu goi rot, thay vi doi nhip heartbeat sau.
static void scheduleBurst() {
  burstLeft = NODE_BURST_COUNT;
  nextBurstMs = millis();
}

// ======================================================================
// SENSOR - debounce rieng tung kenh roi GOP, giong het checkSensors() cua node chinh
// ======================================================================
static void checkSensors() {
  for (int i = 0; i < NODE_SENSOR_NUM; i++) {
    bool active = (digitalRead(nodeSensorPins[i]) == SENSOR_ACTIVE);
    if (active != sensorReading[i]) {
      sensorReading[i] = active;
      sensorTimer[i] = millis();
    }
    if (millis() - sensorTimer[i] >= NODE_DEBOUNCE_MS) {
      sensorState[i] = sensorReading[i];
    }
  }

  bool s1 = sensorState[0];
  bool s2 = sensorState[1];
  int newState;
  if (s1 && s2) newState = GIASACH_STATE_FULL;
  else if (s1 != s2) newState = GIASACH_STATE_ONE_TAKEN;
  else newState = GIASACH_STATE_TWO_TAKEN;

  if (newState != bookState) {
    bookState = newState;
    LOG("Sensor: [%d %d] -> trang thai %d", (int)s1, (int)s2, bookState);
    scheduleBurst();
  }
}

// ======================================================================
// SETUP / LOOP
// ======================================================================
static void espnowInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false); // khong join AP nao ca, chi can radio bat
  // Tat power-save: mac dinh modem-sleep se lam rot goi ESP-NOW den bat chot khi khong join AP.
  esp_wifi_set_ps(WIFI_PS_NONE);

#if ESPNOW_LONG_RANGE
  // Phai bat o CA HAI dau moi noi duoc nhau.
  esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  LOG("ESP-NOW: bat long-range mode");
#endif

  esp_err_t err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) LOG("ESP-NOW: set kenh %d loi %s", ESPNOW_CHANNEL, esp_err_to_name(err));

  LOG("ESP-NOW: MAC node phu   = %s", WiFi.macAddress().c_str());
  LOG("ESP-NOW: MAC node chinh = %02X:%02X:%02X:%02X:%02X:%02X, kenh %d",
      MAIN_NODE_MAC[0], MAIN_NODE_MAC[1], MAIN_NODE_MAC[2],
      MAIN_NODE_MAC[3], MAIN_NODE_MAC[4], MAIN_NODE_MAC[5], ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    LOG("ESP-NOW: init that bai - reset sau 3s");
    delay(3000);
    esp_restart();
  }

  // PMK phai set TRUOC khi them peer ma hoa. Ca hai board dung chung PMK; LMK rieng cho cap
  // node phu <-> node chinh (o day chi co 1 cap nen dung luon 1 LMK).
  static_assert(sizeof(ESPNOW_PMK) == 17, "ESPNOW_PMK phai dung 16 ky tu");
  static_assert(sizeof(ESPNOW_LMK) == 17, "ESPNOW_LMK phai dung 16 ky tu");
  esp_now_set_pmk((const uint8_t*)ESPNOW_PMK);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAIN_NODE_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = true;
  memcpy(peer.lmk, ESPNOW_LMK, 16);
  err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    LOG("ESP-NOW: add peer that bai (%s) - reset sau 3s", esp_err_to_name(err));
    delay(3000);
    esp_restart();
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
}

void setup() {
  Serial.begin(115200);
  unsigned long serialStart = millis();
  while (!Serial && (millis() - serialStart) < 2000) delay(10);

  LOG("=== Gia sach - NODE PHU (sensor qua ESP-NOW) ===");
  LOG("Ly do reset lan truoc: %d (xem esp_reset_reason_t)", (int)esp_reset_reason());

  for (int i = 0; i < NODE_SENSOR_NUM; i++) {
    pinMode(nodeSensorPins[i], INPUT_PULLUP);
    // Nap gia tri raw ban dau de debounce khong phai chay tu trang thai gia (toan FALSE =
    // TRONG) - tranh ban mot cue TWO_TAKEN ma khong ai lay sach that.
    sensorReading[i] = (digitalRead(nodeSensorPins[i]) == SENSOR_ACTIVE);
    sensorState[i]   = sensorReading[i];
    sensorTimer[i]   = millis();
  }

  currentNonce = esp_random();
  espnowInit();

  // Tinh bookState ban dau va ban ngay mot chum: node chinh vua boot (hoac vua mat lien lac)
  // biet lien trang thai hien tai chu khong phai doi den lan doi sensor tiep theo.
  checkSensors();
  if (bookState == GIASACH_STATE_UNKNOWN) bookState = GIASACH_STATE_TWO_TAKEN;
  scheduleBurst();
}

void loop() {
  checkSensors();

  unsigned long now = millis();

  if (rebootRequested) {
    LOG("Reset theo lenh node chinh...");
    Serial.flush();
    delay(100); // de ACK cua goi lenh kip bay ve node chinh
    esp_restart();
  }

  if (pingRequested) {
    pingRequested = false;
    LOG("CMD: PING -> tra loi PONG");
    sendState(GIASACH_FLAG_PONG);
  }

  // Chum goi khi doi trang thai
  if (burstLeft > 0 && (long)(now - nextBurstMs) >= 0) {
    burstLeft--;
    nextBurstMs = now + NODE_BURST_GAP_MS;
    sendState(0);
  }
  // Heartbeat: node chinh dua vao nhip nay de biet node phu con song
  else if ((now - lastHeartbeatMs) >= NODE_HEARTBEAT_MS) {
    sendState(0);
  }

  // Radio treo: da tung gui duoc, roi mat trang qua lau -> tu reset. Dieu kien everSentOk
  // tranh vong lap reboot khi node chinh chua bat (luc do khong gui duoc la binh thuong).
  if (everSentOk && (now - lastSendOkMs) >= NODE_RADIO_STUCK_MS) {
    LOG("ESP-NOW: %lu ms khong gui duoc goi nao - tu reset", (unsigned long)(now - lastSendOkMs));
    Serial.flush();
    esp_restart();
  }

  delay(1); // nhuong CPU cho task WiFi, khong anh huong debounce (500 ms)
}
