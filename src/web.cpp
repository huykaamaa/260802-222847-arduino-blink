#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include <cstring>

// F6-style: gate state-changing endpoints (/save, /test_mqtt, /test_osc, /test_relay)
// behind HTTP Basic Auth. Root GET "/" va polling GET "/data" deliberately khong goi
// ham nay (chi doc, gate se lam hong dashboard tu-refresh).
static bool requireAuth() {
  if (!server.authenticate(authUser, authPass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

static bool parseValidatedLong(const String &s, long minVal, long maxVal, long &out) {
  long v = s.toInt();
  if (v < minVal || v > maxVal) return false;
  out = v;
  return true;
}

void handleData() {
  String data;
  data += "<b>MQTT:</b> ";
  data += mqttConnected ? "<span style='color:green'>CONNECTED</span>" : "<span style='color:red'>DISCONNECTED</span>";
  data += "<br><b>OSC:</b> ";
  data += oscEnabled ? "<span style='color:green'>ENABLED</span>" : "<span style='color:gray'>DISABLED</span>";
  data += "<br><br>";

  for (int i = 0; i < SENSOR_NUM; i++) {
    bool active = (digitalRead(sensorPins[i]) == SENSOR_ACTIVE);
    data += "Vị trí " + String(i + 1) + " : ";
    data += active ? "<span style='color:green;font-weight:bold'>CÓ SÁCH</span>" : "<span style='color:red;font-weight:bold'>TRỐNG</span>";
    data += " | relay ";
    data += relayOutput[i] ? "<b style='color:green'>ON</b>" : "OFF";
    data += ((long)(relayTestUntil[i] - millis()) > 0) ? " (testing)" : "";
    data += "<br>";
  }

  server.send(200, "text/html", data);
}

void handleSave() {
  if (!requireAuth()) return;

  bool needRestartMQTT = false;

  if (server.hasArg("mqtt_ip")) {
    strncpy(mqttServer, server.arg("mqtt_ip").c_str(), sizeof(mqttServer) - 1);
    mqttServer[sizeof(mqttServer) - 1] = '\0';
    needRestartMQTT = true;
  }

  bool mqttPortInvalid = false;
  if (server.hasArg("mqtt_port")) {
    long v;
    if (parseValidatedLong(server.arg("mqtt_port"), 1, 65535, v)) {
      mqttPort = (uint16_t)v;
      needRestartMQTT = true;
    } else {
      mqttPortInvalid = true;
    }
  }

  if (server.hasArg("mqtt_user")) {
    strncpy(mqttUser, server.arg("mqtt_user").c_str(), sizeof(mqttUser) - 1);
    mqttUser[sizeof(mqttUser) - 1] = '\0';
    needRestartMQTT = true;
  }

  mqttEnabled = server.hasArg("mqtt_enable");

  if (server.hasArg("mqtt_pass")) {
    strncpy(mqttPass, server.arg("mqtt_pass").c_str(), sizeof(mqttPass) - 1);
    mqttPass[sizeof(mqttPass) - 1] = '\0';
    needRestartMQTT = true;
  }

  if (server.hasArg("mqtt_topic")) {
    strncpy(mqttTopic, server.arg("mqtt_topic").c_str(), sizeof(mqttTopic) - 1);
    mqttTopic[sizeof(mqttTopic) - 1] = '\0';
  }
  if (server.hasArg("mqtt_full")) {
    strncpy(mqttFullValue, server.arg("mqtt_full").c_str(), sizeof(mqttFullValue) - 1);
    mqttFullValue[sizeof(mqttFullValue) - 1] = '\0';
  }
  if (server.hasArg("mqtt_missing")) {
    strncpy(mqttMissingValue, server.arg("mqtt_missing").c_str(), sizeof(mqttMissingValue) - 1);
    mqttMissingValue[sizeof(mqttMissingValue) - 1] = '\0';
  }

  oscEnabled = server.hasArg("osc_enable");

  if (server.hasArg("osc_ip")) {
    strncpy(oscIp, server.arg("osc_ip").c_str(), sizeof(oscIp) - 1);
    oscIp[sizeof(oscIp) - 1] = '\0';
  }

  bool oscPortInvalid = false;
  if (server.hasArg("osc_port")) {
    long v;
    if (parseValidatedLong(server.arg("osc_port"), 1, 65535, v)) {
      oscPort = (uint16_t)v;
    } else {
      oscPortInvalid = true;
    }
  }

  // Dia chi OSC de trong khong duoc phep - se lam OSC lang le khong gui gi cho trang thai do
  // ma khong co canh bao. De trong = giu nguyen gia tri cu (giong auth_user/auth_pass),
  // khong cho phep xoa trang thanh rong.
  bool oscAddressInvalid = false;
  if (server.hasArg("osc_address_full") && server.arg("osc_address_full").length() > 0) {
    String v = server.arg("osc_address_full");
    if (v[0] != '/') {
      oscAddressInvalid = true;
    } else {
      strncpy(oscAddressFull, v.c_str(), sizeof(oscAddressFull) - 1);
      oscAddressFull[sizeof(oscAddressFull) - 1] = '\0';
    }
  }
  if (server.hasArg("osc_address_missing") && server.arg("osc_address_missing").length() > 0) {
    String v = server.arg("osc_address_missing");
    if (v[0] != '/') {
      oscAddressInvalid = true;
    } else {
      strncpy(oscAddressMissing, v.c_str(), sizeof(oscAddressMissing) - 1);
      oscAddressMissing[sizeof(oscAddressMissing) - 1] = '\0';
    }
  }

  if (server.hasArg("osc_value_full")) oscValueFull = server.arg("osc_value_full").toInt();
  if (server.hasArg("osc_value_missing")) oscValueMissing = server.arg("osc_value_missing").toInt();

  if (server.hasArg("debounce")) {
    long v = server.arg("debounce").toInt();
    if (v < 5) v = 5; else if (v > 60000) v = 60000;
    debounceTime = (unsigned long)v;
  }

  if (server.hasArg("heartbeat")) {
    long v = server.arg("heartbeat").toInt();
    if (v <= 0) v = 0; // 0 = tat
    else if (v < 5000) v = 5000;
    else if (v > 3600000) v = 3600000;
    heartbeatInterval = (unsigned long)v;
  }

  ethUseStaticFirst = server.hasArg("eth_static_first");

  if (server.hasArg("eth_ip")) {
    strncpy(ethStaticIp, server.arg("eth_ip").c_str(), sizeof(ethStaticIp) - 1);
    ethStaticIp[sizeof(ethStaticIp) - 1] = '\0';
  }
  if (server.hasArg("eth_gw")) {
    strncpy(ethStaticGateway, server.arg("eth_gw").c_str(), sizeof(ethStaticGateway) - 1);
    ethStaticGateway[sizeof(ethStaticGateway) - 1] = '\0';
  }
  if (server.hasArg("eth_mask")) {
    strncpy(ethStaticNetmask, server.arg("eth_mask").c_str(), sizeof(ethStaticNetmask) - 1);
    ethStaticNetmask[sizeof(ethStaticNetmask) - 1] = '\0';
  }

  if (server.hasArg("auth_user") && server.arg("auth_user").length() > 0) {
    strncpy(authUser, server.arg("auth_user").c_str(), sizeof(authUser) - 1);
    authUser[sizeof(authUser) - 1] = '\0';
  }
  if (server.hasArg("auth_pass") && server.arg("auth_pass").length() > 0) {
    strncpy(authPass, server.arg("auth_pass").c_str(), sizeof(authPass) - 1);
    authPass[sizeof(authPass) - 1] = '\0';
  }

  for (int i = 0; i < SENSOR_NUM; i++) {
    sensorEnable[i] = server.hasArg("en" + String(i));
  }

  int saveFailCount = saveConfig();

  if (needRestartMQTT) {
    if (mqtt) {
      esp_mqtt_client_stop(mqtt);
      esp_mqtt_client_destroy(mqtt);
      mqtt = NULL;
      mqttConnected = false; // stop()/destroy() khong tu ban MQTT_EVENT_DISCONNECTED
    }
    mqttInit();
  }

  String alertMsg;
  if (saveFailCount == 0) alertMsg = "Saved OK";
  else if (saveFailCount < 0) alertMsg = "Save FAILED - NVS not accessible";
  else alertMsg = "Saved with " + String(saveFailCount) + " error(s)";

  if (oscAddressInvalid) alertMsg += " (OSC address rejected: must start with /)";
  if (mqttPortInvalid) alertMsg += " (MQTT port rejected: must be 1-65535)";
  if (oscPortInvalid) alertMsg += " (OSC port rejected: must be 1-65535)";

  server.send(200, "text/html", "<script>alert('" + alertMsg + "');window.location.href='/';</script>");
}

// Test MQTT/OSC: ban lan luot vi tri 1..6 ON (CO) truoc, roi 1..6 OFF (TRONG), cach nhau
// 1s/buoc - tong 12 buoc. Dung millis(), KHONG dung delay(), de khong block loop().
#define TEST_SEQ_INTERVAL_MS 1000UL
#define TEST_SEQ_TOTAL_STEPS (SENSOR_NUM * 2)

static bool testSeqActive = false;
static int testSeqIndex = 0;
static unsigned long testSeqNextMs = 0;

static void startTestSequence() {
  testSeqActive = true;
  testSeqIndex = 0;
  testSeqNextMs = millis(); // ban buoc dau tien ngay lap tuc
}

void updateTestSequence() {
  if (!testSeqActive) return;
  if ((long)(testSeqNextMs - millis()) > 0) return; // an toan qua vong lap millis()

  int pos = testSeqIndex % SENSOR_NUM;      // 0..5, lap lai o nua sau
  bool state = testSeqIndex < SENSOR_NUM;   // nua dau: ON (CO), nua sau: OFF (TRONG)
  triggerSensor(pos, state);
  testSeqIndex++;

  if (testSeqIndex >= TEST_SEQ_TOTAL_STEPS) {
    testSeqActive = false;
  } else {
    testSeqNextMs = millis() + TEST_SEQ_INTERVAL_MS;
  }
}

void handleTestMQTT() {
  if (!requireAuth()) return;
  startTestSequence();
  server.send(200, "text/html", "<script>window.location.href='/';</script>");
}

void handleTestOSC() {
  if (!requireAuth()) return;
  startTestSequence();
  server.send(200, "text/html", "<script>window.location.href='/';</script>");
}

void handleTestRelay() {
  if (!requireAuth()) return;
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    triggerRelayTest(id);
  }
  server.send(200, "text/html", "<script>window.location.href='/';</script>");
}

// ======================================================================
// NVS CONFIG LOAD/SAVE
// ======================================================================
int saveConfig() {
  if (!prefs.begin(NVS_KEY("shelf"), false)) {
    return -1;
  }
  int fails = 0;
  if (!prefs.putString(NVS_KEY("mqtt_ip"), mqttServer)) fails++;
  if (!prefs.putUShort(NVS_KEY("mqtt_port"), mqttPort)) fails++;
  if (!prefs.putBool(NVS_KEY("mqtt_en"), mqttEnabled)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_user"), mqttUser)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_pass"), mqttPass)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_topic"), mqttTopic)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_full"), mqttFullValue)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_missing"), mqttMissingValue)) fails++;

  if (!prefs.putBool(NVS_KEY("osc_en"), oscEnabled)) fails++;
  if (!prefs.putString(NVS_KEY("osc_ip"), oscIp)) fails++;
  if (!prefs.putUShort(NVS_KEY("osc_port"), oscPort)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_full"), oscAddressFull)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_miss"), oscAddressMissing)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_full"), oscValueFull)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_miss"), oscValueMissing)) fails++;

  if (!prefs.putULong(NVS_KEY("debounce"), debounceTime)) fails++;
  if (!prefs.putULong(NVS_KEY("heartbeat"), heartbeatInterval)) fails++;

  if (!prefs.putString(NVS_KEY("eth_ip"), ethStaticIp)) fails++;
  if (!prefs.putString(NVS_KEY("eth_gw"), ethStaticGateway)) fails++;
  if (!prefs.putString(NVS_KEY("eth_mask"), ethStaticNetmask)) fails++;
  if (!prefs.putBool(NVS_KEY("eth_first"), ethUseStaticFirst)) fails++;

  if (!prefs.putString(NVS_KEY("auth_user"), authUser)) fails++;
  if (!prefs.putString(NVS_KEY("auth_pass"), authPass)) fails++;

  for (int i = 0; i < SENSOR_NUM; i++) {
    if (!prefs.putBool(("en" + String(i)).c_str(), sensorEnable[i])) fails++;
  }

  prefs.end();
  return fails;
}

void loadConfig() {
  if (!prefs.begin(NVS_KEY("shelf"), false)) {
    LOG("NVS: prefs.begin(shelf) FAILED - using in-RAM defaults");
    return;
  }

  strncpy(mqttServer, prefs.getString(NVS_KEY("mqtt_ip"), mqttServer).c_str(), sizeof(mqttServer) - 1);
  mqttServer[sizeof(mqttServer) - 1] = '\0';
  mqttPort = prefs.getUShort(NVS_KEY("mqtt_port"), mqttPort);
  mqttEnabled = prefs.getBool(NVS_KEY("mqtt_en"), mqttEnabled);
  strncpy(mqttUser, prefs.getString(NVS_KEY("mqtt_user"), mqttUser).c_str(), sizeof(mqttUser) - 1);
  mqttUser[sizeof(mqttUser) - 1] = '\0';
  strncpy(mqttPass, prefs.getString(NVS_KEY("mqtt_pass"), mqttPass).c_str(), sizeof(mqttPass) - 1);
  mqttPass[sizeof(mqttPass) - 1] = '\0';
  strncpy(mqttTopic, prefs.getString(NVS_KEY("mqtt_topic"), mqttTopic).c_str(), sizeof(mqttTopic) - 1);
  mqttTopic[sizeof(mqttTopic) - 1] = '\0';
  strncpy(mqttFullValue, prefs.getString(NVS_KEY("mqtt_full"), mqttFullValue).c_str(), sizeof(mqttFullValue) - 1);
  mqttFullValue[sizeof(mqttFullValue) - 1] = '\0';
  strncpy(mqttMissingValue, prefs.getString(NVS_KEY("mqtt_missing"), mqttMissingValue).c_str(), sizeof(mqttMissingValue) - 1);
  mqttMissingValue[sizeof(mqttMissingValue) - 1] = '\0';

  oscEnabled = prefs.getBool(NVS_KEY("osc_en"), oscEnabled);
  strncpy(oscIp, prefs.getString(NVS_KEY("osc_ip"), oscIp).c_str(), sizeof(oscIp) - 1);
  oscIp[sizeof(oscIp) - 1] = '\0';
  oscPort = prefs.getUShort(NVS_KEY("osc_port"), oscPort);
  strncpy(oscAddressFull, prefs.getString(NVS_KEY("osc_addr_full"), oscAddressFull).c_str(), sizeof(oscAddressFull) - 1);
  oscAddressFull[sizeof(oscAddressFull) - 1] = '\0';
  strncpy(oscAddressMissing, prefs.getString(NVS_KEY("osc_addr_miss"), oscAddressMissing).c_str(), sizeof(oscAddressMissing) - 1);
  oscAddressMissing[sizeof(oscAddressMissing) - 1] = '\0';
  oscValueFull = prefs.getInt(NVS_KEY("osc_val_full"), oscValueFull);
  oscValueMissing = prefs.getInt(NVS_KEY("osc_val_miss"), oscValueMissing);

  debounceTime = prefs.getULong(NVS_KEY("debounce"), debounceTime);
  heartbeatInterval = prefs.getULong(NVS_KEY("heartbeat"), heartbeatInterval);

  strncpy(ethStaticIp, prefs.getString(NVS_KEY("eth_ip"), ethStaticIp).c_str(), sizeof(ethStaticIp) - 1);
  ethStaticIp[sizeof(ethStaticIp) - 1] = '\0';
  strncpy(ethStaticGateway, prefs.getString(NVS_KEY("eth_gw"), ethStaticGateway).c_str(), sizeof(ethStaticGateway) - 1);
  ethStaticGateway[sizeof(ethStaticGateway) - 1] = '\0';
  strncpy(ethStaticNetmask, prefs.getString(NVS_KEY("eth_mask"), ethStaticNetmask).c_str(), sizeof(ethStaticNetmask) - 1);
  ethStaticNetmask[sizeof(ethStaticNetmask) - 1] = '\0';
  ethUseStaticFirst = prefs.getBool(NVS_KEY("eth_first"), ethUseStaticFirst);

  strncpy(authUser, prefs.getString(NVS_KEY("auth_user"), authUser).c_str(), sizeof(authUser) - 1);
  authUser[sizeof(authUser) - 1] = '\0';
  strncpy(authPass, prefs.getString(NVS_KEY("auth_pass"), authPass).c_str(), sizeof(authPass) - 1);
  authPass[sizeof(authPass) - 1] = '\0';

  for (int i = 0; i < SENSOR_NUM; i++) {
    sensorEnable[i] = prefs.getBool(("en" + String(i)).c_str(), sensorEnable[i]);
  }

  prefs.end();

  if (strcmp(authUser, "admin") == 0 && strcmp(authPass, "admin") == 0) {
    LOG("AUTH: dang dung mac dinh admin/admin cho /save, /test_mqtt, /test_osc, /test_relay - doi qua Web UI");
  }
}
