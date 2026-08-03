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

// Copy 1 arg dang chuoi vao buffer co dinh, BO QUA neu arg de trong (de trong = giu nguyen gia
// tri cu). Ap dung cho cac truong ma chuoi rong se lam hong chuc nang - vd mqtt_ip rong sinh ra
// URI "mqtt://:1883" khien esp_mqtt_client_init() that bai va MQTT chet han cho toi khi sua lai.
static bool saveStringArg(const char *argName, char *dest, size_t destSize) {
  if (!server.hasArg(argName)) return false;
  String v = server.arg(argName);
  if (v.length() == 0) return false;
  strncpy(dest, v.c_str(), destSize - 1);
  dest[destSize - 1] = '\0';
  return true;
}

// Nhu tren nhung bat buoc parse duoc thanh IPv4. Nhap sai IP/gateway/netmask o day co the lam
// mat luon duong vao Web UI o lan boot sau (neu DHCP cung khong len), nen phai chan tai cho va
// bao len UI thay vi luu am tham.
static bool saveIpArg(const char *argName, char *dest, size_t destSize, bool &invalidFlag) {
  if (!server.hasArg(argName)) return false;
  String v = server.arg(argName);
  if (v.length() == 0) return false;  // de trong = giu nguyen, khong phai loi
  IPAddress parsed;
  if (!parsed.fromString(v)) {
    invalidFlag = true;
    return false;
  }
  strncpy(dest, v.c_str(), destSize - 1);
  dest[destSize - 1] = '\0';
  return true;
}

void handleData() {
  String data;
  data += "<b>MQTT:</b> ";
  if (!mqttEnabled) data += "<span style='color:gray'>DISABLED</span>";
  else data += mqttConnected ? "<span style='color:green'>CONNECTED</span>" : "<span style='color:red'>DISCONNECTED</span>";
  data += "<br><b>OSC:</b> ";
  data += oscEnabled ? "<span style='color:green'>ENABLED</span>" : "<span style='color:gray'>DISABLED</span>";
  data += "<br><br>";

  for (int i = 0; i < SENSOR_NUM; i++) {
    // Hien trang thai DA DEBOUNCE (relayState) chu khong phai digitalRead() thoi diem request:
    // gia tri tho nhay nhanh hon relay/MQTT nen dashboard hay mau thuan voi chinh cot relay
    // ngay ben canh.
    data += "Vị trí " + String(i + 1) + " : ";
    data += relayState[i] ? "<span style='color:green;font-weight:bold'>CÓ SÁCH</span>" : "<span style='color:red;font-weight:bold'>TRỐNG</span>";
    data += " | relay ";
    data += relayOutput[i] ? "<b style='color:green'>ON</b>" : "OFF";
    data += relayTestActive(i) ? " (testing)" : "";
    data += "<br>";
  }

  server.send(200, "text/html", data);
}

void handleSave() {
  if (!requireAuth()) return;

  bool needRestartMQTT = false;

  if (saveStringArg("mqtt_ip", mqttServer, sizeof(mqttServer))) needRestartMQTT = true;

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

  // mqtt_user CO the de rong (broker anonymous) nen khong dung saveStringArg() o day - xoa
  // trang o nay la cach duy nhat de bo credentials, vi o Pass khong con doc lai duoc.
  if (server.hasArg("mqtt_user")) {
    strncpy(mqttUser, server.arg("mqtt_user").c_str(), sizeof(mqttUser) - 1);
    mqttUser[sizeof(mqttUser) - 1] = '\0';
    needRestartMQTT = true;
  }

  // Bat/tat MQTT phai restart client: truoc day chi chan publish, client van ket noi va
  // dashboard van bao CONNECTED du da bo tick Enable.
  bool prevMqttEnabled = mqttEnabled;
  mqttEnabled = server.hasArg("mqtt_enable");
  if (mqttEnabled != prevMqttEnabled) needRestartMQTT = true;

  // Form khong do password that ra HTML nua (tranh lo qua "/" von khong can dang nhap), nen
  // o day de trong = giu nguyen password cu - giong auth_pass.
  if (saveStringArg("mqtt_pass", mqttPass, sizeof(mqttPass))) needRestartMQTT = true;

  saveStringArg("mqtt_topic", mqttTopic, sizeof(mqttTopic));
  saveStringArg("mqtt_full", mqttFullValue, sizeof(mqttFullValue));
  saveStringArg("mqtt_missing", mqttMissingValue, sizeof(mqttMissingValue));

  oscEnabled = server.hasArg("osc_enable");

  saveStringArg("osc_ip", oscIp, sizeof(oscIp));

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

  bool ethAddrInvalid = false;
  saveIpArg("eth_ip", ethStaticIp, sizeof(ethStaticIp), ethAddrInvalid);
  saveIpArg("eth_gw", ethStaticGateway, sizeof(ethStaticGateway), ethAddrInvalid);
  saveIpArg("eth_mask", ethStaticNetmask, sizeof(ethStaticNetmask), ethAddrInvalid);

  saveStringArg("auth_user", authUser, sizeof(authUser));
  saveStringArg("auth_pass", authPass, sizeof(authPass));

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
  if (ethAddrInvalid) alertMsg += " (ETH IP/gateway/netmask rejected: not a valid IPv4 address)";

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

  // Buoc phu cuoi cung: tra ben nhan ve trang thai THAT. Chuoi Test goi thang triggerSensor()
  // nen khong dung vao lastSentState[] - sau buoc "1..6 OFF" ben nhan tin ca 6 vi tri deu
  // TRONG, con thiet bi van nghi minh da gui trang thai cu nen se KHONG tu gui lai. Neu khong
  // resync o day thi lech keo dai toi lan doi trang thai vat ly ke tiep (heartbeat co the dang
  // tat = 0).
  if (testSeqIndex >= TEST_SEQ_TOTAL_STEPS) {
    resyncAllPositions();
    testSeqActive = false;
    return;
  }

  int pos = testSeqIndex % SENSOR_NUM;      // 0..5, lap lai o nua sau
  bool state = testSeqIndex < SENSOR_NUM;   // nua dau: ON (CO), nua sau: OFF (TRONG)
  triggerSensor(pos, state);
  testSeqIndex++;
  testSeqNextMs = millis() + TEST_SEQ_INTERVAL_MS;
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
