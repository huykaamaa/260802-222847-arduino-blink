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
  // 2048: do dem thuc te chuoi tinh trong ham nay da ~1.4KB, chua ke phan dong. reserve(1024)
  // truoc day khong con du - tuc no dang realloc dung cai viec ma no sinh ra de tranh.
  data.reserve(2048);

  // Build timestamp cua chinh lan compile nay - de nhan biet OTA co that su nap ban moi khong
  // (build cu se hien gio/ngay cu tren dashboard sau khi reload).
  // BO dong "Firmware build: __DATE__ __TIME__" (2026-08-10). PlatformIO chi bien dich lai file
  // nao thay doi, ma macro do nam trong web.cpp - lan build nao chi sua file khac thi web.cpp
  // khong duoc dich lai va dau thoi gian DUNG YEN, trong khi firmware thi da doi. No noi doi
  // dung luc duy nhat nguoi ta can no: kiem tra xem OTA da an chua. Dong "FW <md5>" o cuoi ham
  // nay doc tu chinh anh dang chay nen khong bao gio sai - dung dong do.
  data += "<b>MQTT:</b> ";
  if (!mqttEnabled) data += "<span style='color:gray'>DISABLED</span>";
  else data += mqttConnected ? "<span style='color:green'>CONNECTED</span>" : "<span style='color:red'>DISCONNECTED</span>";
  data += "<br><b>OSC:</b> ";
  data += oscEnabled ? "<span style='color:green'>ENABLED</span>" : "<span style='color:gray'>DISABLED</span>";

  // Duong mang dang dung: quan trong voi ca cum vi WiFi la duong DU PHONG - thay dong nay bao
  // "WiFi" nghia la day mang cua board dang co van de, can di kiem tra truoc buoi dien chu
  // khong phai doi den luc no rot not.
  data += "<br><b>Mạng:</b> ";
  if (!netConnected()) {
    data += "<span style='color:red'>KHÔNG CÓ IP</span>";
  } else if (eth_connected) {
    data += "<span style='color:green'>ETH</span> " + netLocalIP().toString();
  } else {
    data += "<span style='color:orange;font-weight:bold'>WiFi (dự phòng)</span> " + netLocalIP().toString();
    data += " &middot; RSSI " + String(WiFi.RSSI()) + " dBm";
  }
  // Tach 2 nguyên nhân: gộp chung thì người đọc suy ra sai nguồn cơn (mất mạng vs. cắm lại dây).
  if (netLossReboots() > 0 || ethReturnReboots() > 0) {
    data += "<br><span style='color:#b45309'>⚠ Đã tự reboot";
    if (netLossReboots() > 0) data += " " + String(netLossReboots()) + " lần vì mất ETH";
    if (netLossReboots() > 0 && ethReturnReboots() > 0) data += ",";
    if (ethReturnReboots() > 0) data += " " + String(ethReturnReboots()) + " lần vì cắm lại ETH";
    data += " (đếm từ lần cắm điện gần nhất)</span>";
  }
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

  // Ma firmware DANG CHAY, tinh tu chinh anh da nap (xem fwIdInit() / globals.h). De o day chu
  // khong o trang cau hinh vi day la khoi duy nhat tu lam moi: sau khi OTA xong va board reboot,
  // so nay tu nhay sang gia tri moi ma khong can F5 - ma OTA tu URL lai khong bao gi khi thanh
  // cong nen day la cach xac nhan nhanh nhat.
  data += "<div style='margin-top:8px;font-size:12px;color:#64748b'>FW <b>";
  data += fwId;
  data += "</b> &middot; " + String(fwSize) + " bytes</div>";

  // Canh bao trong cua so chay thu: day la luc DUY NHAT ma mot cu cup nguon se lam board lui ve
  // firmware cu. Operator can biet de doi them mot phut truoc khi rut dien.
  if (otaPendingVerify()) {
    data += "<div style='margin-top:4px;font-size:12px;color:#b45309'>⏳ Firmware mới đang "
            "<b>chạy thử</b> - chưa xác nhận. Đừng cúp nguồn lúc này, nếu không board sẽ tự quay "
            "về bản cũ.</div>";
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

  wifiEnabled = server.hasArg("wifi_enable");

  // SSID dai hon buffer thi strncpy cat cut am tham va board se di tim mot ten mang khong ton
  // tai - hong theo kieu kho doan nhat. Bao thang len UI thay vi luu mot gia tri sai.
  bool wifiSsidTooLong = server.hasArg("wifi_ssid") &&
                         server.arg("wifi_ssid").length() >= sizeof(wifiSsid);
  if (wifiSsidTooLong) {
    LOG("WiFi: SSID dai %u ky tu, toi da %u - khong luu",
        (unsigned)server.arg("wifi_ssid").length(), (unsigned)(sizeof(wifiSsid) - 1));
  }
  // SSID de trong = giu nguyen (saveStringArg bo qua chuoi rong). Mat khau cung vay - form
  // khong do mat khau WiFi ra HTML, giong mqtt_pass/auth_pass, vi "/" khong yeu cau dang nhap.
  if (!wifiSsidTooLong) saveStringArg("wifi_ssid", wifiSsid, sizeof(wifiSsid));
  // Rieng WiFi pass co them nut xoa trang: mang mo (khong mat khau) la cau hinh hop le, ma
  // "de trong = giu nguyen" thi khong con cach nao go mat khau cu ra.
  if (server.hasArg("wifi_pass_clear")) {
    wifiPass[0] = '\0';
  } else {
    saveStringArg("wifi_pass", wifiPass, sizeof(wifiPass));
  }

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
  if (wifiSsidTooLong) alertMsg += " (WiFi SSID rejected: toi da " + String((int)sizeof(wifiSsid) - 1) + " ky tu)";

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

// true khi request /update bi tu choi vi firmware dang chay chua duoc xac nhan - xem
// handleUpdateUrl() ve ly do. Giu qua ca chuoi chunk giong otaAuthOk, de handleUpdateFinish()
// bao dung nguyen nhan thay vi mot loi Update.h chung chung.
static bool otaBlockedPending = false;

// Reset mem board. Gui response TRUOC roi moi restart (giong handleUpdateFinish) - neu goi
// ESP.restart() ngay thi trinh duyet chi thay ket noi bi cat, khong biet lenh da nhan chua.
void handleUpdateUrl() {
  if (!requireAuth()) return;

  // Firmware dang chay chua duoc xac nhan => esp_ota_begin() se tu choi voi
  // ESP_ERR_OTA_ROLLBACK_INVALID_STATE (xem esp_ota_ops.h). Chan tai day de bao duoc mot cau ro
  // rang kem so giay con lai, thay vi de httpUpdate that bai voi mot ma loi kho doan giua chung.
  if (otaPendingVerify()) {
    String msg = "Firmware hien tai dang CHAY THU, chua duoc xac nhan - chua nap moi duoc. "
                 "Doi khoang " + String(otaConfirmRemainSec()) + " giay roi thu lai.";
    server.send(200, "text/html", "<script>alert('" + msg + "');window.location.href='/';</script>");
    return;
  }

  String alertMsg;
  bool urlOk = true;

  if (server.hasArg("ota_url")) {
    String v = server.arg("ota_url");
    v.trim();

    if (v.length() >= sizeof(otaUrl)) {
      // Tu choi thay vi de strncpy cat cut: URL bi cat mat duoi se tro toi mot duong dan khac
      // van hop le ve cu phap, va loi chi lo ra luc dang tai giua show.
      alertMsg = "URL qua dai (toi da " + String((int)sizeof(otaUrl) - 1) + " ky tu) - khong luu";
      urlOk = false;
    } else if (v.length() > 0 && !v.startsWith("http://")) {
      // https:// bi chan ngay o day thay vi de httpUpdate that bai luc tai - xem globals.h.
      alertMsg = "URL phai bat dau bang http:// (khong ho tro https) - khong luu";
      urlOk = false;
    } else {
      strncpy(otaUrl, v.c_str(), sizeof(otaUrl) - 1);
      otaUrl[sizeof(otaUrl) - 1] = '\0';
    }
  }

  if (urlOk) {
    int saveFailCount = saveConfig();
    if (saveFailCount == 0) {
      alertMsg = "Da luu URL";
    } else if (saveFailCount < 0) {
      alertMsg = "Luu URL FAILED - NVS not accessible, check Serial log";
      urlOk = false;
    } else {
      alertMsg = "Luu voi " + String(saveFailCount) + " loi - check Serial log";
    }
  }

  // Chi nap khi bam dung nut "update" VA URL vua qua duoc kiem tra - bam nap ma URL bi tu choi
  // thi se tai bang gia tri CU con nam trong RAM, tuong da doi ma thuc ra chua.
  bool wantUpdate = urlOk && server.arg("act") == "update" && otaUrl[0] != '\0';

  if (wantUpdate) {
    otaUrlPending = true;
    LOG(">>> OTA URL: da nhan lenh nap tu %s <<<", otaUrl);
    alertMsg = "Dang tai firmware tu link. Board se tu khoi dong lai sau khoang 20-40 giay. "
               "Xem Serial log neu that bai.";
  }

  String page = "<script>alert('" + alertMsg + "');";
  // Doi lau hon han thoi gian tai + reboot roi moi quay ve, khong thi trang tu tai lai giua luc
  // board dang ghi flash va bao loi mang lam nguoi dung tuong update hong.
  page += wantUpdate ? "setTimeout(function(){window.location.href='/';},45000);"
                     : "window.location.href='/';";
  page += "</script>";
  server.send(200, "text/html", page);
}

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
    // Chan TRUOC Update.begin(): trong cua so chay thu, esp_ota_begin() se that bai voi
    // ESP_ERR_OTA_ROLLBACK_INVALID_STATE. De no chay toi day roi moi hong thi nguoi dung nhan
    // duoc mot ma loi vo nghia sau khi da cho upload xong ca 1.2MB.
    otaBlockedPending = otaPendingVerify();
    if (otaBlockedPending) {
      LOG("OTA: tu choi upload - firmware hien tai dang chay thu, con %u giay", (unsigned)otaConfirmRemainSec());
      return;
    }
    LOG("OTA: bat dau nhan file '%s'", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaAuthOk || otaBlockedPending) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaAuthOk || otaBlockedPending) return;
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
  bool blocked = otaBlockedPending;
  otaAuthOk = false;
  otaBlockedPending = false;

  if (!authed) {
    server.requestAuthentication();
    return;
  }
  if (blocked) {
    String msg = "Firmware hien tai dang CHAY THU, chua duoc xac nhan - chua nap moi duoc. "
                 "Doi khoang " + String(otaConfirmRemainSec()) + " giay roi thu lai.";
    server.send(200, "text/html", "<script>alert('" + msg + "');window.location.href='/';</script>");
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

  if (!prefs.putBool(NVS_KEY("wifi_en"), wifiEnabled)) fails++;
  // Ca 2 chuoi nay hop le khi RONG (chua nhap SSID / mang mo khong mat khau) - xem giai thich
  // o mqtt_user ben tren ve viec putString() tra 0 byte cho chuoi rong ke ca khi ghi thanh cong.
  if (prefs.putString(NVS_KEY("wifi_ssid"), wifiSsid) == 0 && strlen(wifiSsid) > 0) fails++;
  if (prefs.putString(NVS_KEY("wifi_pass"), wifiPass) == 0 && strlen(wifiPass) > 0) fails++;

  if (!prefs.putString(NVS_KEY("auth_user"), authUser)) fails++;
  if (!prefs.putString(NVS_KEY("auth_pass"), authPass)) fails++;

  if (!prefs.putString(NVS_KEY("ota_url"), otaUrl)) fails++;

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

  wifiEnabled = prefs.getBool(NVS_KEY("wifi_en"), wifiEnabled);
  strncpy(wifiSsid, prefs.getString(NVS_KEY("wifi_ssid"), wifiSsid).c_str(), sizeof(wifiSsid) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = '\0';
  strncpy(wifiPass, prefs.getString(NVS_KEY("wifi_pass"), wifiPass).c_str(), sizeof(wifiPass) - 1);
  wifiPass[sizeof(wifiPass) - 1] = '\0';

  strncpy(authUser, prefs.getString(NVS_KEY("auth_user"), authUser).c_str(), sizeof(authUser) - 1);
  authUser[sizeof(authUser) - 1] = '\0';
  strncpy(authPass, prefs.getString(NVS_KEY("auth_pass"), authPass).c_str(), sizeof(authPass) - 1);
  authPass[sizeof(authPass) - 1] = '\0';

  strncpy(otaUrl, prefs.getString(NVS_KEY("ota_url"), "").c_str(), sizeof(otaUrl) - 1);
  otaUrl[sizeof(otaUrl) - 1] = '\0';

  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    relayPairEnable[p] = prefs.getBool(("relay_pair" + String(p)).c_str(), relayPairEnable[p]);
  }

  prefs.end();

  if (strcmp(authUser, "admin") == 0 && strcmp(authPass, "admin") == 0) {
    LOG("AUTH: dang dung mac dinh admin/admin cho /save, /test_iot, /test_relay, /update, /reboot - doi qua Web UI");
  }
}
