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

static void sendOscAt(const char *address, int value) {
  if (!oscEnabled || strlen(oscIp) == 0 || oscPort == 0) return;
  if (address[0] != '/') return; // OSC address pattern phai bat dau bang '/'
  if (!oscUdp.beginPacket(oscIp, oscPort)) return;
  writeOscString(oscUdp, address);
  writeOscString(oscUdp, ",i");
  writeOscInt32(oscUdp, value);
  oscUdp.endPacket();
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
  mqttConnected = false;

  // KHONG chan theo mqttEnabled o day: bo tick "Enable MQTT" chi chan publish (xem
  // triggerBookState()), client van ket noi toi broker nhu binh thuong.
  if (strlen(mqttServer) == 0) {
    LOG("MQTT: dia chi broker rong - khong khoi tao client");
    return;
  }

  static char uriBuf[96];
  snprintf(uriBuf, sizeof(uriBuf), "mqtt://%s:%u", mqttServer, mqttPort);

  esp_mqtt_client_config_t config;
  memset(&config, 0, sizeof(config));
  config.broker.address.uri = uriBuf;

  static char client_id[] = "GIA_SACH";
  config.credentials.client_id = client_id;

  // Chi gui credentials khi co username. Password khong xoa duoc tu web (o Pass de trong =
  // giu nguyen), nen xoa trang o User la cach de chuyen han sang ket noi anonymous.
  if (strlen(mqttUser) > 0) {
    config.credentials.username = mqttUser;
    if (strlen(mqttPass) > 0) config.credentials.authentication.password = mqttPass;
  }

  mqtt = esp_mqtt_client_init(&config);
  if (mqtt) {
    esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqttEvent, NULL);
    esp_mqtt_client_start(mqtt);
  } else {
    LOG("MQTT init failed");
  }
}

// Bao trang thai sach - moi trang thai (FULL/ONE_TAKEN/TWO_TAKEN) co topic MQTT + dia chi OSC
// rieng, ban 1 lan khi VUA CHUYEN vao trang thai do.
void triggerBookState(int state) {
  const char *topic;
  const char *value;
  const char *oscAddr;
  int oscValue;

  switch (state) {
    case BOOK_STATE_ONE_TAKEN:
      topic = mqttTopicOneTaken; value = mqttValueOneTaken;
      oscAddr = oscAddressOneTaken; oscValue = oscValueOneTaken;
      break;
    case BOOK_STATE_TWO_TAKEN:
      topic = mqttTopicTwoTaken; value = mqttValueTwoTaken;
      oscAddr = oscAddressTwoTaken; oscValue = oscValueTwoTaken;
      break;
    case BOOK_STATE_FULL:
    default:
      topic = mqttTopicFull; value = mqttValueFull;
      oscAddr = oscAddressFull; oscValue = oscValueFull;
      break;
  }

  if (mqttEnabled) {
    if (mqtt && mqttConnected) {
      // enqueue() khong block (khac publish() co the treo loop() khi broker dang reconnect)
      esp_mqtt_client_enqueue(mqtt, topic, value, 0, 0, 0, true);
    } else {
      LOG("MQTT publish skipped: not connected/initialized");
    }
  }
  sendOscAt(oscAddr, oscValue);

  LOG("Trang thai sach = %d (%s)", state, value);
}
