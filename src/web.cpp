#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include <cstring>
#include <Update.h>

// F6-style: gate state-changing endpoints (/save, /test_iot, /test_relay, /update)
// behind HTTP Basic Auth. Root GET "/" va polling GET "/data" deliberately khong goi
// ham nay (chi doc, gate se lam hong dashboard tu-refresh).
static bool requireAuth() {
  if (!server.authenticate(authUser, authPass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// Chuoi PHAI la so thap phan thuan (khong dau, khong ky tu thua) va nam trong [minVal,maxVal].
// Truoc day chi dua vao toInt(), von cat duoi cho chuoi lai ("8080xyz" -> 8080) va tra ve 0 cho
// rac ("abc") - hien tai 2 cho goi deu co minVal = 1 nen rac bi loai, nhung bat ky caller moi
// nao cho phep 0 se am tham nhan rac thanh so 0.
static bool parseValidatedLong(const String &s, long minVal, long maxVal, long &out) {
  if (s.length() == 0 || s.length() > 10) return false; // >10 chu so la chac chan tran long
  for (size_t i = 0; i < s.length(); i++) {
    if (!isdigit((unsigned char)s[i])) return false;
  }
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

static const char* bookStateLabel(int state) {
  switch (state) {
    case BOOK_STATE_FULL:      return "<span style='color:green;font-weight:bold'>ĐỦ SÁCH</span>";
    case BOOK_STATE_ONE_TAKEN: return "<span style='color:orange;font-weight:bold'>ĐÃ LẤY 1 CUỐN</span>";
    case BOOK_STATE_TWO_TAKEN: return "<span style='color:red;font-weight:bold'>ĐÃ LẤY 2 CUỐN</span>";
    default:                   return "<span style='color:gray'>...</span>";
  }
}

void handleData() {
  String data;
  // Trang dashboard poll route nay 2 lan/giay VINH VIEN khi co tab mo. Khong reserve() thi moi
  // lan goi la mot chuoi realloc tang dan, bo lai block chet giua heap - dung cai nguy co phan
  // manh ma globals.h vien dan de doi config sang char[].
  data.reserve(1024);

  // Build timestamp cua chinh lan compile nay - de nhan biet OTA co that su nap ban moi khong
  // (build cu se hien gio/ngay cu tren dashboard sau khi reload).
  data += "<span style='font-size:12px;color:#94a3b8'>Firmware build: " __DATE__ " " __TIME__ "</span><br>";
  data += "<b>MQTT:</b> ";
  if (!mqttEnabled) data += "<span style='color:gray'>DISABLED</span>";
  else data += mqttConnected ? "<span style='color:green'>CONNECTED</span>" : "<span style='color:red'>DISCONNECTED</span>";
  data += "<br><b>OSC:</b> ";
  data += oscEnabled ? "<span style='color:green'>ENABLED</span>" : "<span style='color:gray'>DISABLED</span>";
  data += "<br><br>";

  data += "<b>Trạng thái:</b> ";
  data += bookStateLabel(bookState);
  data += "<br><br>";

  for (int i = 0; i < SENSOR_NUM; i++) {
    // Hien trang thai DA DEBOUNCE (sensorState) chu khong phai digitalRead() thoi diem
    // request: gia tri tho nhay nhanh hon relay/MQTT nen dashboard hay mau thuan.
    data += "Sensor " + String(i + 1) + " : ";
    data += sensorState[i] ? "<span style='color:green;font-weight:bold'>CÓ SÁCH</span>" : "<span style='color:red;font-weight:bold'>TRỐNG</span>";
    data += "<br>";
  }
  bool testing = relayTestActive();
  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    data += "Cặp " + String(p + 1) + (relayPairEnable[p] ? " (đang bật)" : " (tắt)") + " - Relay " + String(2 * p + 1) + " : ";
    data += relayOutput[2 * p] ? "<b style='color:green'>ON</b>" : "OFF";
    data += testing && relayPairEnable[p] ? " (testing)" : "";
    data += " | Relay " + String(2 * p + 2) + " : ";
    data += relayOutput[2 * p + 1] ? "<b style='color:green'>ON</b>" : "OFF";
    data += testing && relayPairEnable[p] ? " (testing)" : "";
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
  // Bo tick "Enable MQTT" CHI chan publish, KHONG ngat client - ket noi toi broker duoc giu
  // nguyen (chu dich cua chu du an). Vi vay khong set needRestartMQTT o day.
  mqttEnabled = server.hasArg("mqtt_enable");

  // Form khong do password that ra HTML nua (tranh lo qua "/" von khong can dang nhap), nen
  // o day de trong = giu nguyen password cu - giong auth_pass.
  if (saveStringArg("mqtt_pass", mqttPass, sizeof(mqttPass))) needRestartMQTT = true;

  saveStringArg("mqtt_topic_full", mqttTopicFull, sizeof(mqttTopicFull));
  saveStringArg("mqtt_val_full", mqttValueFull, sizeof(mqttValueFull));
  saveStringArg("mqtt_topic_one", mqttTopicOneTaken, sizeof(mqttTopicOneTaken));
  saveStringArg("mqtt_val_one", mqttValueOneTaken, sizeof(mqttValueOneTaken));
  saveStringArg("mqtt_topic_two", mqttTopicTwoTaken, sizeof(mqttTopicTwoTaken));
  saveStringArg("mqtt_val_two", mqttValueTwoTaken, sizeof(mqttValueTwoTaken));

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
  auto saveOscAddress = [&](const char *argName, char *dest, size_t destSize) {
    if (!server.hasArg(argName) || server.arg(argName).length() == 0) return;
    String v = server.arg(argName);
    if (v[0] != '/') {
      oscAddressInvalid = true;
      return;
    }
    strncpy(dest, v.c_str(), destSize - 1);
    dest[destSize - 1] = '\0';
  };
  saveOscAddress("osc_address_full", oscAddressFull, sizeof(oscAddressFull));
  saveOscAddress("osc_address_one", oscAddressOneTaken, sizeof(oscAddressOneTaken));
  saveOscAddress("osc_address_two", oscAddressTwoTaken, sizeof(oscAddressTwoTaken));

  if (server.hasArg("osc_value_full")) oscValueFull = server.arg("osc_value_full").toInt();
  if (server.hasArg("osc_value_one")) oscValueOneTaken = server.arg("osc_value_one").toInt();
  if (server.hasArg("osc_value_two")) oscValueTwoTaken = server.arg("osc_value_two").toInt();

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

  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    relayPairEnable[p] = server.hasArg("relay_pair" + String(p));
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

// Test MQTT/OSC: ban lan luot ca 3 trang thai (FULL -> ONE_TAKEN -> TWO_TAKEN), cach nhau
// 1s/buoc. Dung millis(), KHONG dung delay(), de khong block loop().
#define TEST_SEQ_INTERVAL_MS 1000UL
#define TEST_SEQ_TOTAL_STEPS 3

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

  // Buoc phu cuoi cung: tra ben nhan ve trang thai sach THAT. Chuoi Test goi thang
  // triggerBookState() nen khong dung bookState - sau buoc cuoi ben nhan tin trang thai la
  // TWO_TAKEN, con thiet bi van nghi minh da gui trang thai cu nen se KHONG tu gui lai. Neu
  // khong resync o day thi lech keo dai toi lan doi trang thai vat ly ke tiep (heartbeat co
  // the dang tat = 0).
  if (testSeqIndex >= TEST_SEQ_TOTAL_STEPS) {
    resyncBookState();
    testSeqActive = false;
    return;
  }

  triggerBookState(testSeqIndex + 1); // 1=FULL, 2=ONE_TAKEN, 3=TWO_TAKEN
  testSeqIndex++;
  testSeqNextMs = millis() + TEST_SEQ_INTERVAL_MS;
}

// MOT route duy nhat cho ca 2 kenh. Truoc day co /test_mqtt va /test_osc rieng nhung than ham
// y het nhau tung chu (khong co ca dong LOG nao de phan biet), vi chuoi test goi
// triggerBookState() - von ban CA MQTT LAN OSC. Hai nut rieng gay hieu nham la test duoc tung
// kenh mot: dang soi "MQTT khong toi noi" ma bam Test MQTT roi thay ben nhan OSC phan hoi thi
// rat de ket luan nham la MQTT on.
void handleTestIot() {
  if (!requireAuth()) return;
  startTestSequence();
  server.send(200, "text/html", "<script>window.location.href='/';</script>");
}

void handleTestRelay() {
  if (!requireAuth()) return;
  triggerRelayTest();
  server.send(200, "text/html", "<script>window.location.href='/';</script>");
}

// ======================================================================
// OTA (upload .bin qua web) - dung thu vien Update.h co san trong ESP32 core, khong can them
// thu vien nao. Partition table mac dinh cua board (default_8MB.csv) da co san 2 slot OTA
// (app0/app1), khong can dong gi o platformio.ini.
// ======================================================================

// true trong suot 1 request /update tu luc auth duoc kiem tra (o buoc START) - dung chung giua
// handleUpdateUpload() (goi nhieu lan trong luc nhan tung chunk) va handleUpdateFinish() (goi 1
// lan sau khi nhan xong) vi Auth chi kiem tra duoc 1 lan luc bat dau, khong the goi lai
// requireAuth() o giua chung (header da xu ly xong).
static bool otaAuthOk = false;

// Reset mem board. Gui response TRUOC roi moi restart (giong handleUpdateFinish) - neu goi
// ESP.restart() ngay thi trinh duyet chi thay ket noi bi cat, khong biet lenh da nhan chua.
void handleReboot() {
  if (!requireAuth()) return;
  server.send(200, "text/html",
    "<script>alert('Board dang khoi dong lai. Doi khoang 15-20 giay roi tai lai trang.');"
    "setTimeout(function(){window.location.href='/';},10000);</script>");
  delay(500); // cho response gui xong truoc khi reboot
  ESP.restart();
}

void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Kiem tra auth NGAY tu chunk dau, truoc khi ghi byte nao vao flash - khong doi den
    // handleUpdateFinish() (luc do da nhan/bo qua toan bo file, phi bang thong + thoi gian).
    otaAuthOk = server.authenticate(authUser, authPass);
    if (!otaAuthOk) {
      LOG("OTA: tu choi upload - sai auth");
      return;
    }
    LOG("OTA: bat dau nhan file '%s'", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaAuthOk) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaAuthOk) return;
    if (Update.end(true)) {
      LOG("OTA: nhan xong %u bytes", (unsigned)upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    LOG("OTA: upload bi huy giua chung");
  }
}

// Chay SAU khi handleUpdateUpload() da nhan xong toan bo file (hoac tu choi tu dau vi sai
// auth). Bao ket qua len trinh duyet roi tu reboot neu ghi flash thanh cong.
void handleUpdateFinish() {
  // Reset ngay khi doc: otaAuthOk la static nen no song qua nhieu request. Mot POST /update
  // KHONG kem file thi handleUpdateUpload() khong chay lan nao, va co se con giu gia tri cua
  // lan OTA truoc. Khong khai thac duoc de ghi flash (chunk START luon authenticate() lai
  // truoc khi ghi byte nao) nhung khong co ly do gi de co do song sot qua request.
  bool authed = otaAuthOk;
  otaAuthOk = false;

  if (!authed) {
    server.requestAuthentication();
    return;
  }
  bool ok = !Update.hasError();
  server.send(200, "text/html", ok
    ? "<script>alert('OTA thanh cong - dang khoi dong lai...');window.location.href='/';</script>"
    : "<script>alert('OTA THAT BAI - xem log Serial, board van chay firmware cu.');window.location.href='/';</script>");
  if (ok) {
    delay(500); // cho response gui xong truoc khi reboot
    ESP.restart();
  }
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
  // putString() tra ve SO BYTE DA GHI - voi chuoi RONG (vd mqtt anonymous: User/Pass de trong
  // co chu dich), gia tri do luon la 0 ke ca khi ghi THANH CONG, nen "== 0" khong dung duoc de
  // bao loi trong truong hop nay. Van goi putString binh thuong (de luu dung gia tri rong vao
  // NVS), chi bo qua viec dem fail khi nguon von di da rong tu dau.
  if (prefs.putString(NVS_KEY("mqtt_user"), mqttUser) == 0 && strlen(mqttUser) > 0) fails++;
  if (prefs.putString(NVS_KEY("mqtt_pass"), mqttPass) == 0 && strlen(mqttPass) > 0) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_topic_full"), mqttTopicFull)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_val_full"), mqttValueFull)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_topic_one"), mqttTopicOneTaken)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_val_one"), mqttValueOneTaken)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_topic_two"), mqttTopicTwoTaken)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_val_two"), mqttValueTwoTaken)) fails++;

  if (!prefs.putBool(NVS_KEY("osc_en"), oscEnabled)) fails++;
  if (!prefs.putString(NVS_KEY("osc_ip"), oscIp)) fails++;
  if (!prefs.putUShort(NVS_KEY("osc_port"), oscPort)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_full"), oscAddressFull)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_full"), oscValueFull)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_one"), oscAddressOneTaken)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_one"), oscValueOneTaken)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_two"), oscAddressTwoTaken)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_two"), oscValueTwoTaken)) fails++;

  if (!prefs.putULong(NVS_KEY("debounce"), debounceTime)) fails++;
  if (!prefs.putULong(NVS_KEY("heartbeat"), heartbeatInterval)) fails++;

  if (!prefs.putString(NVS_KEY("eth_ip"), ethStaticIp)) fails++;
  if (!prefs.putString(NVS_KEY("eth_gw"), ethStaticGateway)) fails++;
  if (!prefs.putString(NVS_KEY("eth_mask"), ethStaticNetmask)) fails++;
  if (!prefs.putBool(NVS_KEY("eth_first"), ethUseStaticFirst)) fails++;

  if (!prefs.putString(NVS_KEY("auth_user"), authUser)) fails++;
  if (!prefs.putString(NVS_KEY("auth_pass"), authPass)) fails++;

  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    if (!prefs.putBool(("relay_pair" + String(p)).c_str(), relayPairEnable[p])) fails++;
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
  strncpy(mqttTopicFull, prefs.getString(NVS_KEY("mqtt_topic_full"), mqttTopicFull).c_str(), sizeof(mqttTopicFull) - 1);
  mqttTopicFull[sizeof(mqttTopicFull) - 1] = '\0';
  strncpy(mqttValueFull, prefs.getString(NVS_KEY("mqtt_val_full"), mqttValueFull).c_str(), sizeof(mqttValueFull) - 1);
  mqttValueFull[sizeof(mqttValueFull) - 1] = '\0';
  strncpy(mqttTopicOneTaken, prefs.getString(NVS_KEY("mqtt_topic_one"), mqttTopicOneTaken).c_str(), sizeof(mqttTopicOneTaken) - 1);
  mqttTopicOneTaken[sizeof(mqttTopicOneTaken) - 1] = '\0';
  strncpy(mqttValueOneTaken, prefs.getString(NVS_KEY("mqtt_val_one"), mqttValueOneTaken).c_str(), sizeof(mqttValueOneTaken) - 1);
  mqttValueOneTaken[sizeof(mqttValueOneTaken) - 1] = '\0';
  strncpy(mqttTopicTwoTaken, prefs.getString(NVS_KEY("mqtt_topic_two"), mqttTopicTwoTaken).c_str(), sizeof(mqttTopicTwoTaken) - 1);
  mqttTopicTwoTaken[sizeof(mqttTopicTwoTaken) - 1] = '\0';
  strncpy(mqttValueTwoTaken, prefs.getString(NVS_KEY("mqtt_val_two"), mqttValueTwoTaken).c_str(), sizeof(mqttValueTwoTaken) - 1);
  mqttValueTwoTaken[sizeof(mqttValueTwoTaken) - 1] = '\0';

  oscEnabled = prefs.getBool(NVS_KEY("osc_en"), oscEnabled);
  strncpy(oscIp, prefs.getString(NVS_KEY("osc_ip"), oscIp).c_str(), sizeof(oscIp) - 1);
  oscIp[sizeof(oscIp) - 1] = '\0';
  oscPort = prefs.getUShort(NVS_KEY("osc_port"), oscPort);
  strncpy(oscAddressFull, prefs.getString(NVS_KEY("osc_addr_full"), oscAddressFull).c_str(), sizeof(oscAddressFull) - 1);
  oscAddressFull[sizeof(oscAddressFull) - 1] = '\0';
  oscValueFull = prefs.getInt(NVS_KEY("osc_val_full"), oscValueFull);
  strncpy(oscAddressOneTaken, prefs.getString(NVS_KEY("osc_addr_one"), oscAddressOneTaken).c_str(), sizeof(oscAddressOneTaken) - 1);
  oscAddressOneTaken[sizeof(oscAddressOneTaken) - 1] = '\0';
  oscValueOneTaken = prefs.getInt(NVS_KEY("osc_val_one"), oscValueOneTaken);
  strncpy(oscAddressTwoTaken, prefs.getString(NVS_KEY("osc_addr_two"), oscAddressTwoTaken).c_str(), sizeof(oscAddressTwoTaken) - 1);
  oscAddressTwoTaken[sizeof(oscAddressTwoTaken) - 1] = '\0';
  oscValueTwoTaken = prefs.getInt(NVS_KEY("osc_val_two"), oscValueTwoTaken);

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

  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    relayPairEnable[p] = prefs.getBool(("relay_pair" + String(p)).c_str(), relayPairEnable[p]);
  }

  prefs.end();

  if (strcmp(authUser, "admin") == 0 && strcmp(authPass, "admin") == 0) {
    LOG("AUTH: dang dung mac dinh admin/admin cho /save, /test_iot, /test_relay, /update, /reboot - doi qua Web UI");
  }
}
