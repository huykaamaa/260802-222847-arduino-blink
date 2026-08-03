#include "globals.h"
#include "mqtt.h"
#include <cstring>

// ======================================================================
// OSC (UDP, tu encode OSC 1.0 - khong dung lib ngoai)
// ======================================================================
static void writeOscString(WiFiUDP &udp, const char *text) {
  size_t len = strlen(text);
  udp.write(reinterpret_cast<const uint8_t*>(text), len);
  // luon pad it nhat 1 byte NUL, ke ca khi len da chia het cho 4
  // ("(4 - len%4) % 4" se ra 0 padding trong truong hop do va lam mat terminator)
  size_t padding = 4 - (len % 4);
  for (size_t i = 0; i < padding; ++i) {
    udp.write((uint8_t)0);
  }
}

static void writeOscInt32(WiFiUDP &udp, int32_t value) {
  // OSC 1.0 yeu cau int32 big-endian ("network byte order"); ESP32 native little-endian
  // nen phai tu dao byte thu cong.
  uint32_t be = static_cast<uint32_t>(value);
  uint8_t bytes[4] = {
    static_cast<uint8_t>(be >> 24),
    static_cast<uint8_t>(be >> 16),
    static_cast<uint8_t>(be >> 8),
    static_cast<uint8_t>(be)
  };
  udp.write(bytes, sizeof(bytes));
}

// addressTemplate dung chung cho ca 6 vi tri, "{id}" duoc thay bang vi tri (1..6) truoc khi gui
// vd "/composition/layers/1/clips/{id}/connect" -> vi tri 3 se gui "/composition/layers/1/clips/3/connect"
static void sendOscAt(const char *addressTemplate, int id, int value) {
  if (!oscEnabled || strlen(oscIp) == 0 || oscPort == 0) return;
  String addr = addressTemplate;
  addr.replace("{id}", String(id));
  if (addr.length() == 0 || addr[0] != '/') return; // OSC address pattern phai bat dau bang '/'
  if (!oscUdp.beginPacket(oscIp, oscPort)) return;
  writeOscString(oscUdp, addr.c_str());
  writeOscString(oscUdp, ",i");
  writeOscInt32(oscUdp, value);
  oscUdp.endPacket();
}

// CO va TRONG la 2 message OSC doc lap - dia chi rieng + gia tri rieng cho tung trang thai,
// khong dung 1 dia chi roi doi int 1/0.
static void sendOscPosition(int id, bool state) {
  if (state) {
    sendOscAt(oscAddressFull, id, oscValueFull);
  } else {
    sendOscAt(oscAddressMissing, id, oscValueMissing);
  }
}

// ======================================================================
// MQTT
// ======================================================================
void mqttEvent(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      mqttConnected = true;
      LOG("MQTT Connected");
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;
      LOG("MQTT Disconnected");
      break;
    default:
      break;
  }
}

void mqttInit() {
  static char uriBuf[96];
  snprintf(uriBuf, sizeof(uriBuf), "mqtt://%s:%u", mqttServer, mqttPort);

  esp_mqtt_client_config_t config;
  memset(&config, 0, sizeof(config));
  config.broker.address.uri = uriBuf;

  static char client_id[] = "GIA_SACH";
  config.credentials.client_id = client_id;

  if (strlen(mqttUser) > 0) config.credentials.username = mqttUser;
  if (strlen(mqttPass) > 0) config.credentials.authentication.password = mqttPass;

  mqtt = esp_mqtt_client_init(&config);
  if (mqtt) {
    esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqttEvent, NULL);
    esp_mqtt_client_start(mqtt);
  } else {
    mqttConnected = false;
    LOG("MQTT init failed");
  }
}

// Bao trang thai RIENG tung vi tri - moi vi tri 1 topic MQTT rieng "<mqttTopic>/<id>",
// OSC dung 2 dia chi rieng (CO/TRONG), id chen vao dia chi qua "{id}".
void triggerSensor(int id, bool state) {
  const char *value = state ? mqttFullValue : mqttMissingValue;

  if (mqttEnabled) {
    if (mqtt && mqttConnected) {
      // enqueue() khong block (khac publish() co the treo loop() khi broker dang reconnect)
      String topic = String(mqttTopic) + "/" + String(id + 1);
      esp_mqtt_client_enqueue(mqtt, topic.c_str(), value, 0, 0, 0, true);
    } else {
      LOG("MQTT publish skipped: not connected/initialized");
    }
  }
  sendOscPosition(id + 1, state);

  LOG("Vi tri %d = %s", id + 1, value);
}
