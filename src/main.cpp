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
#include <HTTPUpdate.h>     // OTA tu URL - nam trong core Arduino-ESP32, khong phai lib ngoai
#include <esp_ota_ops.h>    // trang thai anh OTA + xac nhan/rollback - xem otaRollbackTick()
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
// ======================================================================
// LOG VONG - xem globals.h
// ======================================================================
// 60 dong x 128 ky tu = 7.7KB RAM tinh. Doi lai la doc duoc dien bien khoi dong cua mot board
// nam cach xa vai chuc cay so, thu ma truoc gio chi co cap USB moi thay duoc.
//
// 60 dong du chua tron ven mot lan boot (chuoi cho ETH -> thu WiFi -> MQTT) - do la doan can
// doc nhat. Dai hon nua thi cai can xem bi day ra khoi dem boi log chay dinh ky luc sau.
#define LOG_LINES 60
#define LOG_LINE_MAX 128
static char logBuf[LOG_LINES][LOG_LINE_MAX];
// uint32_t chu khong phai uint16_t: dem den 65535 roi vong ve 0 se lam logDump() tinh sai moc
// bat dau va in ra thu tu lung tung. 32 bit thi voi nhip log cua board nay la hang chuc nam.
static uint32_t logCount = 0;   // tong so dong da ghi (khong reset khi vong lai)
// LOG() duoc goi ca tu loop() lan tu WiFiEvent() - von chay trong task su kien cua he thong -
// nen hai luong co the ghi cung luc. Spinlock chi bao doan CHEP vao dem; phan dinh dang chuoi
// (vsnprintf, cham) de ben ngoai de khong khoa lau.
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

void logPrintf(const char *fmt, ...) {
  char line[LOG_LINE_MAX];

  // Dau moi dong la so giay tu luc boot. Khong co no thi mot dem log chi la mot dong chu roi
  // rac, khong biet cai nao xay ra truoc cai nao va cach nhau bao lau.
  unsigned long ms = millis();
  int n = snprintf(line, sizeof(line), "[%lu.%lus] ", ms / 1000UL, (ms % 1000UL) / 100UL);
  if (n < 0 || n >= (int)sizeof(line)) n = 0;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n, fmt, ap);
  va_end(ap);

  Serial.println(line);   // duong Serial giu nguyen, khong mat gi

  portENTER_CRITICAL(&logMux);
  strncpy(logBuf[logCount % LOG_LINES], line, LOG_LINE_MAX - 1);
  logBuf[logCount % LOG_LINES][LOG_LINE_MAX - 1] = '\0';
  logCount++;
  portEXIT_CRITICAL(&logMux);
}

String logDump() {
  String out;
  out.reserve(LOG_LINES * 64);

  portENTER_CRITICAL(&logMux);
  uint32_t total = logCount;
  portEXIT_CRITICAL(&logMux);

  // Chua vong het mot luot thi chi co `total` dong; vong roi thi dong cu nhat nam ngay sau con
  // tro ghi. Doc ngoai vung khoa: du co mot dong bi ghi de giua chung thi cung chi lam ban dung
  // dong do, khong dang de khoa ca vong lap chep chuoi.
  uint32_t start = (total > LOG_LINES) ? (total - LOG_LINES) : 0;
  for (uint32_t i = start; i < total; i++) {
    out += logBuf[i % LOG_LINES];
    out += '\n';
  }
  if (total == 0) out = "(chua co dong log nao)\n";
  return out;
}

SPIClass spi(FSPI);
WebServer server(80);
Preferences prefs;
WiFiUDP oscUdp;
bool eth_connected = false;
bool wifi_connected = false;

// Xem globals.h. ETH luon duoc uu tien; netLinkName() tra "ETH" khi ca hai cung len (thuc te
// khong xay ra vi chi thu WiFi sau khi ETH that bai, tru khi cam lai day mang giua chung).
// true tu luc WiFi du phong ket noi duoc lan dau. Khac wifi_connected o cho no KHONG tut ve
// false khi song rot: cac cho phai quyet dinh "co duoc tat radio WiFi khong" (mode cua diag AP)
// can biet "board nay dang song bang WiFi", chu khong phai "ngay giay nay co song hay khong" -
// dung wifi_connected o do thi mot nhip mat song dung luc tat diag AP se tat luon radio va
// khong bao gio ket noi lai duoc.
static bool wifiFallbackActive = false;

// Da bat WiFi STA trong phien nay chua - dat true NGAY KHI quyet dinh dung WiFi, khong doi ket
// noi thanh cong. Khac han wifiFallbackActive ("dang thuc su chay bang WiFi").
//
// Su khac biet nay la thu quyet dinh WiFi co duoc thu lai hay khong. Truoc day ca hai cho chon
// che do radio deu doc wifiFallbackActive, nen khi lan ket noi dau THAT BAI thi startDiagAp()
// dat WIFI_AP (tháo bo STA) roi 5 phut sau dat WIFI_OFF (tat han radio) - WiFi khong bao gio
// duoc thu lai nua du mang co len tro lai. Canh de dinh nhat: board va router cam chung mot o,
// bat cung luc, board khoi dong nhanh hon nen luc no thu thi AP chua kip phat song.
static bool wifiStaActive = false;

// ETH da tung len IP trong phien nay chua. ethUpAtBoot chot lai KET QUA CUA setup() va khong
// bao gio doi nua - ethReturnTick() phai dua vao no chu khong phai vao trang thai song:
// khi cam lai day, ETH tu xin DHCP va eth_connected bat len chi sau vai giay, neu doc trang
// thai song thi dieu kien tu tat truoc khi kip reboot, tuc no se khong bao gio reboot.
static bool ethUpAtBoot = false;
static bool ethEverUp = false;

// Co day mang dang cam va bat tay xong chua. PHAI theo doi bang su kien ETH_CONNECTED /
// ETH_DISCONNECTED chu KHONG duoc dung ETH.linkUp(): ham do thuc chat la
// NetworkInterface::linkUp(), than ham chi co "return esp_netif_is_netif_up(_esp_netif)" - tuc
// no bao netif da duoc dung len hay chua, hoan toan khong doc trang thai PHY. Ap mot bo IP tinh
// cung lam netif up, nen dung no de hoi "co day khong" thi cau tra loi la "co" ke ca khi day
// dang nam tren ban.
static volatile bool ethLinkPresent = false;

// ETH co bang chung THUC SU noi duoc voi mang chua: DHCP cap duoc IP (chung to co server tra
// loi) hoac gateway tra loi ping. Bo IP tinh ap vao mot cong khong cam day cung tao ra mot dia
// chi trong dep de nhung khong ai toi duoc - co nay de phan biet hai truong hop do tren
// dashboard va tren ten diag AP, thay vi bao "STATIC 192.168.99.5" nhu the moi thu deu on.
static bool ethVerified = false;

// So lan da tu reboot vi mang, xem netWatchdogTick() / ethReturnTick(). Muc dich cua bo dem la
// chan vong lap reboot, nen no BAT BUOC phai song qua ESP.restart() - bien thuong thi moi lan
// reboot lai ve 0 va cai tran tro thanh vo nghia.
//
// PHAI la RTC_NOINIT_ATTR chu KHONG phai RTC_DATA_ATTR. Xem esp_attr.h: RTC_DATA_ATTR chi hua
// giu gia tri "during a deep sleep / wake cycle" - no nam trong .rtc.data, von duoc bootloader
// nap lai tu anh firmware o moi lan boot binh thuong (reset mem la mot lan boot binh thuong),
// nen bo dem bi xoa sach. Chi .rtc_noinit moi duoc bo qua khi nap va giu nguyen "after restart".
//
// Gia phai tra cua noinit: luc CUP NGUON roi cam lai, vung nay chua RAC chu khong phai 0 - phai
// co magic word de biet luc nao duoc phep tin bo dem.
#define NET_REBOOT_MAGIC 0x47534831UL   // "GSH1" - doi gia tri nay neu doi y nghia cac bo dem
RTC_NOINIT_ATTR static uint32_t netRebootMagic;
RTC_NOINIT_ATTR static uint32_t netRebootCount;

// De rieng chu khong dung chung bo dem voi netRebootCount: hai su kien nay noi tiep nhau trong
// dung mot kich ban thuong gap (mat ETH -> reboot -> chay WiFi -> cam lai day -> reboot), dung
// chung mot tran dem thi buoc thu hai bi chan boi chinh buoc thu nhat.
RTC_NOINIT_ATTR static uint32_t ethReturnRebootCount;

// Goi mot lan o dau setup(), TRUOC moi cho doc hai bo dem tren.
static void netRebootCountersInit() {
  if (netRebootMagic != NET_REBOOT_MAGIC) {
    netRebootMagic = NET_REBOOT_MAGIC;
    netRebootCount = 0;
    ethReturnRebootCount = 0;
    LOG("Net: khoi tao bo dem reboot (cup nguon hoac nap firmware moi)");
  }
}

uint32_t netLossReboots() { return netRebootCount; }
uint32_t ethReturnReboots() { return ethReturnRebootCount; }

bool netConnected() { return eth_connected || wifi_connected; }
IPAddress netLocalIP() { return eth_connected ? ETH.localIP() : WiFi.localIP(); }
const char* netLinkName() { return eth_connected ? "ETH" : (wifi_connected ? "WiFi" : "-"); }

// Co bang chung thuc su noi duoc voi mang hay khong. eth_connected chi co nghia "da co mot bo
// IP gan vao netif" - dieu do van dung khi day mang nam tren ban.
bool netVerified() { return wifi_connected || (eth_connected && ethVerified); }

// Diagnostic AP - phat wifi de doc IP tren dien thoai thay vi phai mo Serial.
//
// 2026-08-21: KHONG con tu tat sau 5 phut. Ly do: nhin tu xa, chi bang danh sach WiFi tren
// dien thoai, phai phan biet duoc "board chay va co mang" (thay ten kem IP), "board chay
// nhung mat mang" (thay ten NOLINK/OFFLINE) va "board mat dien" (khong thay gi ca). Tat sau
// 5 phut thi hai truong hop sau nhin y het nhau.
static const char *DIAG_AP_PASS = "12121212";
bool diagApActive = false;
// Ten dang phat. Giu lai de biet khi nao no khong con dung su that nua - xem diagApTick().
static String diagApSsid;
// Nhanh nao da cap IP: true = IP tinh du phong. diagApTick() can no de dung tien to cu khi
// phat lai ten, nen phai o pham vi file chu khong con la bien cuc bo trong setup().
static bool ethUsedFallback = false;

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

// Xem globals.h
char fwId[9] = "";
uint32_t fwSize = 0;
char otaUrl[96] = "";
bool otaUrlPending = false;

// ETH static-IP fallback
char ethStaticIp[16]      = "192.168.99.200";
char ethStaticGateway[16] = "192.168.99.1";
char ethStaticNetmask[16] = "255.255.255.0";
bool ethUseStaticFirst = false;

// DNS DU PHONG khi chay IP TINH (2026-08-21).
//
// Nhanh IP tinh von chi dien dns1 = gateway va bo trong dns2. Do la mot GIA DINH: rang cai
// gateway ay co chay dich vu DNS, va rang no chiu tra loi cho THIET BI NAY. Gia dinh do da vo
// o mot mang that - board hoi gateway roi im, trong khi mot laptop cung dat IP tinh tren dung
// mang do, hoi dung gateway do, lai tra ve ket qua binh thuong. Khong giai thich duoc bang
// cach doc source, ma cung khong can: dien mot DNS cong cong vao o dns2 dang bo trong thi lwIP
// tu hoi sang no khi cai dau khong tra loi.
//
// Chi la duong LUI - dns1 van la gateway nhu cu, mang binh thuong khong doi hanh vi gi. Va no
// chi cuu duoc khi board that su ra duoc Internet; mang show khep kin thi van chiu, nhung o do
// ten mien cung vo dung san roi nen khong mat gi.
static const IPAddress DNS_FALLBACK(8, 8, 8, 8);

// WiFi du phong - xem globals.h. Mac dinh TAT: bat len ma chua nhap SSID chi lam boot cham
// them mot cach vo ich.
bool wifiEnabled = false;
char wifiSsid[33] = "";
char wifiPass[64] = "";

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
// CHO DOI MA VAN CHAY SENSOR/RELAY
// ======================================================================
// Moi cho doi mang trong setup() deu phai dung ham nay thay cho delay(). Ly do: relay duoc dat
// OFF het o dau setup(), va relay 1 theo thiet ke phai LUON ON - neu chan bang delay() tran thi
// suot ca giai doan do mang (cho link + ping + DHCP + WiFi) relay 1 nam im o OFF va sensor
// khong duoc doc lan nao, trong khi phan sensor/relay von chang lien quan gi toi mang.
static void checkSensors();

static void netDelay(unsigned long ms) {
  unsigned long t0 = millis();
  do {
    checkSensors();
    delay(5);
  } while ((millis() - t0) < ms);
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
      // Day su kien PHY that su bao "da co day va bat tay xong" - xem ethLinkPresent.
      LOG("ETH Connected (co day mang)");
      ethLinkPresent = true;
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      // Chi den tu DHCP. Co IP do nghia la co server tra loi, tuc mang that.
      LOG("ETH Got IP: %s", ETH.localIP().toString().c_str());
      eth_connected = true;
      ethVerified = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      LOG("ETH Disconnected (mat day mang)");
      ethLinkPresent = false;
      eth_connected = false;
      ethVerified = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      LOG("ETH Stopped");
      ethLinkPresent = false;
      eth_connected = false;
      ethVerified = false;
      break;

    // WiFi du phong: chi co y nghia khi da roi vao nhanh fallback, nhung van bat su kien vo
    // dieu kien - re hon la co them mot bien "dang o nhanh wifi" phai giu dong bo.
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      LOG("WiFi Got IP: %s", WiFi.localIP().toString().c_str());
      wifi_connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // Chi log lan DAU mat ket noi: WiFi.setAutoReconnect() ban su kien nay lai moi vai giay
      // trong suot thoi gian router tat, do Serial ra hang nghin dong giong het nhau.
      if (wifi_connected) LOG("WiFi Disconnected");
      wifi_connected = false;
      break;
    default:
      break;
  }
}

// ======================================================================
// WIFI DU PHONG - chi goi khi ETH da that bai. Xem globals.h.
// ======================================================================
static bool wifiTryConnect() {
  if (!wifiEnabled) {
    LOG("WiFi: khong bat du phong WiFi - bo qua");
    return false;
  }
  if (wifiSsid[0] == '\0') {
    LOG("WiFi: da bat du phong nhung chua nhap SSID - bo qua");
    return false;
  }

  WiFi.mode(WIFI_STA);
  wifiStaActive = true;  // tu day tro di radio phai duoc giu song - xem khai bao

  // PHAI config() TRUOC begin(): goi sau thi esp_netif da khoi dong DHCP client, bo IP tinh
  // khong duoc ap. Dung dung bo IP/gw/mask cua ETH - xem globals.h ve ly do dung chung.
  if (ethUseStaticFirst) {
    IPAddress ip, gw, mask;
    if (ip.fromString(ethStaticIp) && gw.fromString(ethStaticGateway) && mask.fromString(ethStaticNetmask)) {
      if (WiFi.config(ip, gw, mask, gw, DNS_FALLBACK)) { // gw lam DNS1, 8.8.8.8 lam DNS2
        LOG("WiFi: dung IP tinh chung voi ETH - %s (gw %s, mask %s)", ethStaticIp, ethStaticGateway, ethStaticNetmask);
      } else {
        LOG("WiFi: WiFi.config() that bai - de WiFi xin DHCP");
      }
    } else {
      LOG("WiFi: IP tinh khong parse duoc - de WiFi xin DHCP");
    }
  }

  // Tu ket noi lai khi router chop nguon / song chap chon. Khong co dong nay thi mot lan rot
  // song la mat mang vinh vien cho toi luc reboot.
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false); // SSID/pass da nam trong NVS cua ta, khong can core ghi ban thu hai

  LOG("WiFi: dang ket noi toi SSID '%s'...", wifiSsid);
  WiFi.begin(wifiSsid, wifiPass[0] ? wifiPass : nullptr);

  const unsigned long WIFI_WAIT_MS = 20000UL;
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_WAIT_MS) {
    netDelay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Khong tat STA o day: wifiRetryTick() se tiep tuc thu lai dinh ky. Router len cham hon
    // board la chuyen thuong, khong co ly do gi de bo cuoc sau dung mot lan thu.
    LOG("WiFi: khong ket noi duoc sau %lu ms (status=%d) - kiem tra SSID/mat khau, se tu thu lai dinh ky",
        WIFI_WAIT_MS, (int)WiFi.status());
    return false;
  }

  // Su kien GOT_IP thuong da chay xong truoc khi thoat vong tren, nhung voi IP tinh thi
  // esp_netif khong ban GOT_IP theo cung nhip - set thang o day cho chac.
  wifi_connected = true;
  wifiFallbackActive = true;
  LOG("WiFi: da ket noi - IP %s", WiFi.localIP().toString().c_str());
  return true;
}

// Thu ket noi WiFi lai theo chu ky, cho toi khi duoc. Goi moi vong loop().
//
// Khong dua vao mot minh setAutoReconnect(): co do xu ly viec RONG mat ket noi sau khi DA
// associate duoc, con o day co the la chua bao gio associate lan nao (sai mat khau luc dau,
// hoac AP chua phat song luc board thu). Mot lenh begin() moi la cach chac chan nhat.
static void wifiRetryTick() {
  const unsigned long WIFI_RETRY_MS = 60000UL;

  if (!wifiStaActive) return;  // phien nay khong dung WiFi

  if (wifi_connected) {
    // Ket noi duoc o mot lan thu lai (khong phai o wifiTryConnect luc boot) - tu day board that
    // su chay bang WiFi, cac cho khac doc co nay de biet dieu do.
    if (!wifiFallbackActive) {
      wifiFallbackActive = true;
      // Chot netif mac dinh o day nua, khong chi trong setup(): duong nay la khi lan thu luc
      // boot THAT BAI va mai sau moi vao duoc, tuc doan trong setup() da chay xong tu lau voi
      // wifi_connected con la false. Bo sot thi board vao duoc mang ma van khong ra noi
      // Internet - xem giai thich day du o setup().
      WiFi.STA.setDefault();
      LOG("WiFi: da ket noi o lan thu lai - IP %s (netif mac dinh: ETH=%d STA=%d)",
          WiFi.localIP().toString().c_str(), (int)ETH.isDefault(), (int)WiFi.STA.isDefault());
    }
    return;
  }

  // ETH len duoc thi thoi, no la duong chinh. ethReturnTick() se lo viec quay ve ETH cho tu te.
  if (eth_connected) return;

  // Khoi tao bang millis() nen nhip dau tien cach lan thu trong setup() dung mot chu ky, khong
  // ban ngay o vong loop() dau.
  static unsigned long lastTry = millis();
  if ((millis() - lastTry) < WIFI_RETRY_MS) return;
  lastTry = millis();

  LOG("WiFi: thu ket noi lai toi '%s'", wifiSsid);
  WiFi.begin(wifiSsid, wifiPass[0] ? wifiPass : nullptr);
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
  while (!ethLinkPresent && (millis() - t0) < LINK_WAIT_MS) {
    netDelay(50);
  }
  if (!ethLinkPresent) {
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
    netDelay(20);
  }
  esp_ping_stop(hdl);
  esp_ping_delete_session(hdl);

  return gwPingGotReply;
}

// Ten diag AP nen la gi NGAY LUC NAY. Tach rieng khoi startDiagAp() de diagApTick() dung lai
// duoc - AP phat mai nen ten phai theo kip trang thai, xem diagApTick().
//
// Tag toi da 8 ky tu: SSID chuan 802.11 gioi han 32, ma "GIASACH-" (8) + tag + "-" (1) + IPv4
// dai nhat (15) = 24 + tag.
static String diagApName() {
  const char *tag;
  if (!netConnected())     tag = "NOLINK";
  else if (!eth_connected) tag = "WIFI";
  else if (!ethVerified)   tag = "OFFLINE";
  else if (ethUsedFallback) tag = "STATIC";
  else                     tag = "DHCP";

  // Khong co duong mang nao thi khong co IP de dan vao ten AP - de tran tag ("NOLINK"), dung
  // dan "0.0.0.0" vao lam operator tuong day la mot dia chi that.
  String ssid = String("GIASACH-") + tag;
  if (netConnected()) ssid += "-" + netLocalIP().toString();
  // Chan cung theo gioi han SSID cua 802.11. Khong cat thi softAP() that bai va operator mat
  // duong vao cuoi cung - hong am tham dung luc moi thu khac cung dang hong.
  if (ssid.length() > 32) ssid = ssid.substring(0, 32);
  return ssid;
}

// Phat 1 AP co SSID chua trang thai + IP hien tai, de doc tren dien thoai thay vi phai mo
// Serial. Co mat khau DIAG_AP_PASS. KHONG con tu tat - xem khai bao diagApSsid.
static void startDiagAp() {
  String ssid = diagApName();
  // Da bat STA thi PHAI la AP_STA: WIFI_AP thuan se tat luon STA. Dieu kien la wifiStaActive
  // chu khong phai wifiFallbackActive - neu chi giu STA khi da ket noi duoc thi truong hop
  // "chua ket noi duoc, dang cho thu lai" se bi cat dut ngay tai day.
  WiFi.mode(wifiStaActive ? WIFI_AP_STA : WIFI_AP);
  if (WiFi.softAP(ssid.c_str(), DIAG_AP_PASS)) {
    diagApActive = true;
    diagApSsid = ssid;
    LOG("Diag AP: broadcasting %s", ssid.c_str());
  } else {
    LOG("Diag AP: softAP() failed");
  }
}

// Ten AP phai theo kip trang thai. Truoc day 5 phut la du ngan de khong kip doi gi; gio no phat
// mai nen mot cai ten sai se sai mai - va day dung la cai ten operator dua vao de ket luan tu
// xa. Rut day mang ra thi ten phai doi thanh NOLINK, cam lai thi phai quay ve dia chi that.
//
// Chi goi lai softAP() khi ten THUC SU khac: no da may dang noi vao AP ra, khong lam bua duoc.
static void diagApTick() {
  if (!diagApActive) return;
  String want = diagApName();
  if (want == diagApSsid) return;
  LOG("Diag AP: trang thai doi %s -> %s", diagApSsid.c_str(), want.c_str());
  startDiagAp();
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
// Xem globals.h. Goi 1 lan trong setup(); log ra Serial luon de doi chieu duoc voi dashboard
// ma khong can mo trinh duyet.
void fwIdInit()
{
  String md5 = ESP.getSketchMD5();
  strncpy(fwId, md5.c_str(), sizeof(fwId) - 1);
  fwId[sizeof(fwId) - 1] = '\0';
  fwSize = ESP.getSketchSize();
  LOG("FW: id=%s size=%u bytes", fwId, (unsigned)fwSize);
}

// ======================================================================
// OTA ROLLBACK GUARD - xem globals.h ve ly do ton tai
// ======================================================================
// Cua so chay thu. Ngan co chu dich: su co can bat la "firmware moi chet ngay khi khoi dong",
// von lo ra trong vai giay dau. Keo dai cua so ra chi lam tang kha nang mot lan CUP NGUON binh
// thuong bi hieu nham thanh that bai va lam board lui ve ban cu mot cach oan uong. 60 giay du
// de qua het setup() (cho mang toi ~40s o truong hop xau) cong them mot doan loop() that su.
static const unsigned long OTA_CONFIRM_MS = 60000UL;

// Ghi de weak symbol trong core (esp32-hal-misc.c). PHAI la extern "C": ben do la file .c nen
// symbol khong bi mangle, dinh nghia kieu C++ se khong ghi de duoc ma tao ra mot symbol khac.
// Tra ve true = "khoan xac nhan", initArduino() se khong tu goi
// esp_ota_mark_app_valid_cancel_rollback() nua ma de viec do cho otaRollbackTick().
extern "C" bool verifyRollbackLater() { return true; }

static bool otaPending = false;    // anh dang chay o trang thai PENDING_VERIFY
static bool otaConfirmed = false;  // da tu xac nhan xong
static bool otaConfirmStarted = false;
static unsigned long otaConfirmT0 = 0;

bool otaPendingVerify() { return otaPending && !otaConfirmed; }

// Con bao nhieu giay nua thi xac nhan. Dung cho thong bao tren Web UI - trong cua so nay moi
// thao tac OTA deu bi SDK tu choi (xem handleUpdateUrl), nen phai noi duoc cho nguoi dung biet
// con phai doi bao lau thay vi chi bao "khong duoc".
uint32_t otaConfirmRemainSec() {
  if (!otaPendingVerify()) return 0;
  if (!otaConfirmStarted) return OTA_CONFIRM_MS / 1000UL;
  unsigned long elapsed = millis() - otaConfirmT0;
  if (elapsed >= OTA_CONFIRM_MS) return 0;
  return (uint32_t)((OTA_CONFIRM_MS - elapsed + 999UL) / 1000UL);
}

void otaRollbackInit() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    // Doc khong duoc trang thai thi coi nhu khong co gi de xac nhan. Khong tu dat otaPending =
    // true o day: lam vay se goi mark_app_valid() vo co tren mot anh nap bang USB (von khong
    // nam trong quy trinh OTA), gay nhieu hon la loi ich.
    LOG("OTA: khong doc duoc trang thai anh dang chay - bo qua buoc xac nhan");
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;  // anh cu, da xac nhan tu lan boot truoc

  otaPending = true;
  LOG("OTA: firmware nay dang CHAY THU - se tu xac nhan sau %lu giay chay lien tuc. "
      "Neu no chet hoac bi cup nguon truoc do, lan boot sau bootloader tu lui ve ban cu.",
      OTA_CONFIRM_MS / 1000UL);
}

void otaRollbackTick() {
  if (!otaPending || otaConfirmed) return;

  // Bat dau dem o LAN GOI DAU TIEN, tuc vong loop() dau tien - khong phai luc boot. Chu dich la
  // dem thoi gian thuc su chay binh thuong, khong tinh ca doan setup() ngoi cho mang.
  if (!otaConfirmStarted) {
    otaConfirmStarted = true;
    otaConfirmT0 = millis();
  }
  if ((millis() - otaConfirmT0) < OTA_CONFIRM_MS) return;

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    otaConfirmed = true;
    LOG("OTA: da xac nhan firmware nay chay tot - se khong lui ve ban cu nua");
  } else {
    // Thu lai o vong sau. Khong dat otaConfirmed de tranh mac ket vinh vien o trang thai chay
    // thu neu day chi la mot loi nhat thoi.
    LOG("OTA: xac nhan that bai (err=%d) - se thu lai", (int)err);
  }
}

// Tai firmware tu otaUrl roi tu ghi flash + reboot. Xem globals.h ve ly do chi ho tro http://
// va ve rui ro bao mat cua viec keo firmware tu URL.
void otaUrlTick()
{
  // Cho them mot nhip sau khi handler dat co, roi moi tai. handleUpdateUrl() da goi
  // server.send() nhung byte cuoi chua chac da roi khoi socket; nhay vao update ngay se chan
  // loop ~20 giay roi reboot, trinh duyet mat ket noi va bao loi du update chay dung.
  static bool armed = false;
  static unsigned long armedAt = 0;
  const unsigned long OTA_URL_SETTLE_MS = 500UL;

  if (!otaUrlPending) {
    armed = false;
    return;
  }
  if (!armed) {
    armed = true;
    armedAt = millis();
    return;
  }
  if (millis() - armedAt < OTA_URL_SETTLE_MS) {
    return;
  }

  otaUrlPending = false;
  armed = false;

  if (otaUrl[0] == '\0') {
    LOG("OTA URL: chua luu URL nao - bo qua");
    return;
  }

  LOG(">>> OTA URL: bat dau tai %s <<<", otaUrl);

  // Tu phan giai ten host TRUOC khi giao cho httpUpdate, chi de GHI LAI dia chi ra log.
  //
  // Ly do: khi that bai, httpUpdate chi tra ve -1 "connection refused" - ma ma do dung chung
  // cho CA HAI truong hop "tra DNS hong" lan "mo TCP khong duoc" (xem NetworkClient::connect:
  // hostByName that bai thi tra 0, va HTTPClient doi 0 thanh -1). Nen mot minh ma loi do khong
  // noi duoc gi. Dong log nay tach bach hai kha nang, va quan trong hon: cho biet board dang
  // goi toi DIA CHI NAO - neu ten mien bi mang loc va tra ve mot dia chi khac thi chi o day moi
  // lo ra.
  //
  // Ket qua duoc lwIP cache nen lan tra cua httpUpdate ngay sau do khong ton them.
  {
    const char *h = otaUrl;
    if (strncmp(h, "http://", 7) == 0) h += 7;
    char host[64];
    size_t n = 0;
    while (h[n] && h[n] != ':' && h[n] != '/' && n < sizeof(host) - 1) { host[n] = h[n]; n++; }
    host[n] = '\0';

    IPAddress resolved;
    unsigned long t0 = millis();
    if (Network.hostByName(host, resolved)) {
      LOG("OTA URL: '%s' -> %s (%lu ms)", host, resolved.toString().c_str(), millis() - t0);
    } else {
      LOG("OTA URL: KHONG phan giai duoc '%s' sau %lu ms - hong o DNS, chua he mo TCP",
          host, millis() - t0);
    }
  }

  NetworkClient client;
  httpUpdate.rebootOnUpdate(true);
  // Server file tinh hay tra 301/302 (vd thieu dau / cuoi duong dan); khong bat theo redirect
  // thi bao "HTTP error 302" rat kho doan ra nguyen nhan.
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(client, otaUrl);
  switch (ret) {
    case HTTP_UPDATE_OK:
      // Thuc te khong bao gio in ra: rebootOnUpdate(true) restart ngay trong update().
      LOG("OTA URL: xong - dang reboot");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      LOG("OTA URL: server khong tra ve firmware moi");
      break;
    case HTTP_UPDATE_FAILED:
      LOG("OTA URL: THAT BAI (%d) %s", httpUpdate.getLastError(),
          httpUpdate.getLastErrorString().c_str());
      break;
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long serialStart = millis();
  while (!Serial && (millis() - serialStart) < 2000) delay(10);

  fwIdInit();
  netRebootCountersInit();
  otaRollbackInit();

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

  // Mo socket OSC o DAY, khong som hon va khong muon hon.
  //
  // Khong duoc som hon: mo socket doi hoi lwIP da duoc dung len, ma viec do chi xay ra ben
  // trong ETH.begin() (no goi Network.begin()). Goi truoc do thi lwIP lay mot semaphore NULL va
  // board chet ngay o setup(): "assert failed: xQueueSemaphoreTake queue.c:1709" roi reboot -
  // lap vo tan vi lan boot nao cung chet o dung cho do.
  //
  // Khong duoc muon hon (truoc day no nam sau server.begin()): netDelay() trong giai doan cho
  // mang ben duoi da chay checkSensors(), va lan xac dinh trang thai sach dau tien goi thang
  // sang triggerBookState() -> sendOscAt(). NetworkUDP::beginPacket() KHONG tu mo socket, viec
  // do nam o endPacket(), nen cue OSC dau tien se ban vao socket chua mo.
  oscUdp.begin(OSC_LOCAL_PORT);

  // Toan bo giai doan "thu ETH" bi chan trong 20 giay - het thoi gian do ma van chua co IP thi
  // chuyen sang WiFi du phong (neu co bat). Deadline tinh mot lan o day de moi buoc ben duoi
  // (cho link, cho DHCP) deu an chung mot ngan sach thoi gian, khong cong don thanh vai phut.
  const unsigned long ETH_TOTAL_WAIT_MS = 20000UL;
  const unsigned long ethDeadline = millis() + ETH_TOTAL_WAIT_MS;

  bool usedFallback = false;

  // Cho LINK Ethernet truoc moi lam gi khac. Truoc day khong co buoc nay: khi rut day mang,
  // nhanh "static fallback" ben duoi van ap IP tinh thanh cong va dat eth_connected = true, nen
  // board tuong minh dang o tren mang du chang noi voi ai duoc - va nhu the thi nhanh WiFi se
  // KHONG BAO GIO chay. Khong co link = khong co Ethernet, du IP dep den may.
  bool ethLinkUp = false;
  while (!(ethLinkUp = ethLinkPresent) && (long)(ethDeadline - millis()) > 0) {
    netDelay(100);
  }
  if (!ethLinkUp) {
    LOG("ETH: khong co link sau %lu ms (chua cam day mang?) - chuyen sang WiFi du phong", ETH_TOTAL_WAIT_MS);
  }

  // Uu tien IP tinh: ap dung ngay, bo qua hoan toan cho DHCP (boot nhanh hon, dung khi mang
  // khong co DHCP server hoac muon IP co dinh chac chan). Ap xong PHAI ping gateway de xac
  // minh bo IP nay thuc su noi chuyen duoc voi mang - xem gatewayReachable() ve ly do.
  if (ethLinkUp && ethUseStaticFirst) {
    IPAddress ip, gw, mask;
    if (ip.fromString(ethStaticIp) && gw.fromString(ethStaticGateway) && mask.fromString(ethStaticNetmask)) {
      // Truyen gateway lam DNS1: ETH.config() 3 tham so de DNS trong, nen neu MQTT broker
      // duoc nhap bang hostname thay vi IP thi se khong resolve duoc o nhanh IP tinh.
      if (ETH.config(ip, gw, mask, gw, DNS_FALLBACK)) {
        if (gatewayReachable(gw)) {
          eth_connected = true;
          ethVerified = true;   // gateway tra loi = co mang that
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

  if (ethLinkUp && !eth_connected) {
    // Cho DHCP 10s nhu cu, nhung khong duoc vuot qua ngan sach 1 phut cua ca giai doan ETH
    // (nhanh uu tien-IP-tinh phia tren co the da an mat vai giay cho link + ping gateway).
    const unsigned long ETH_WAIT_MS = 10000UL;
    unsigned long ethStart = millis();
    while (!eth_connected && (millis() - ethStart) < ETH_WAIT_MS && (long)(ethDeadline - millis()) > 0) {
      netDelay(100);
    }

    if (!eth_connected) {
      LOG("ETH khong len IP sau %lu ms, ap dung static fallback de Web UI van truy cap duoc", ETH_WAIT_MS);
      IPAddress fallbackIp, fallbackGw, fallbackMask;
      if (fallbackIp.fromString(ethStaticIp) && fallbackGw.fromString(ethStaticGateway) && fallbackMask.fromString(ethStaticNetmask)) {
        if (ETH.config(fallbackIp, fallbackGw, fallbackMask, fallbackGw, DNS_FALLBACK)) { // gw lam DNS1, 8.8.8.8 lam DNS2
          eth_connected = true;
          usedFallback = true;
          // Ping o day nua chu khong chi o nhanh uu tien-IP-tinh: khong hoi dap thi bo IP nay
          // chi la mot dia chi trong dep de ma khong ai toi duoc. Van GIU IP (Web UI con vao
          // duoc neu sau do co nguoi cam day) nhung KHONG bao la da vao mang.
          ethVerified = gatewayReachable(fallbackGw);
          LOG("ETH: static fallback IP %s (gw %s, mask %s) - gateway %s",
              ethStaticIp, ethStaticGateway, ethStaticNetmask,
              ethVerified ? "tra loi ping, mang OK" : "KHONG tra loi - CHUA VAO DUOC MANG");
        } else {
          LOG("ETH: static fallback ETH.config() that bai");
        }
      } else {
        LOG("ETH: static fallback IP/gw/mask khong parse duoc - kiem tra cau hinh Mang");
      }
    }
  }

  // Het duong ETH ma van chua co IP -> thu WiFi. Day la duong DU PHONG, khong phai duong song
  // song: neu ETH da len thi khong dung toi WiFi de khoi phai chon xem MQTT/OSC di loi nao.
  if (!eth_connected) {
    wifiTryConnect();
  }

  // Chot lai ket qua cua ca giai doan tren - xem giai thich o cho khai bao ethUpAtBoot.
  ethUpAtBoot = eth_connected;
  ethEverUp = eth_connected;

  // CHOT NETIF MAC DINH ve dung duong dang chay.
  //
  // Netif mac dinh la thu quyet dinh goi tin di dau khi dich KHONG nam trong subnet cua minh -
  // tuc MOI THU ra ngoai Internet. Dich trong cung subnet thi lwIP tu khop theo dia chi, khong
  // can den no; nen mot netif mac dinh sai cho ra dung trieu chung da gap: Web UI chay, MQTT
  // noi bo chay, DNS toi gateway chay, ma khong mo noi TCP toi bat ky dia chi Internet nao.
  //
  // esp_netif tu chon netif mac dinh dua tren su kien GOT_IP. Ma dat IP TINH thi GOT_IP KHONG
  // duoc ban ra - chinh comment trong wifiTryConnect() da ghi nhan dieu do. Core Arduino cung
  // khong bao gio tu goi setDefault(). Board nay lai la board DUY NHAT trong cum co HAI netif
  // (ETH khong cam day + WiFi), nen no la board duy nhat co the chot nham.
  //
  // Goi thang, khong dua vao suy doan cua ai ca.
  // In trang thai TRUOC khi ep, roi moi ep, roi in lai. Chi in sau khi ep thi con so luon dep
  // va khong chung minh duoc gi - cai can biet la esp_netif DA chon nham hay chua, tuc gia
  // thuyet nay dung hay sai.
  LOG("Netif mac dinh TRUOC khi ep: ETH=%d STA=%d", (int)ETH.isDefault(), (int)WiFi.STA.isDefault());
  if (eth_connected) {
    ETH.setDefault();
  } else if (wifi_connected) {
    WiFi.STA.setDefault();
  }
  LOG("Netif mac dinh SAU khi ep:  ETH=%d STA=%d", (int)ETH.isDefault(), (int)WiFi.STA.isDefault());

  // In DNS dang thuc su dung. Truoc day khong in o dau ca, nen khi "nap tu link bang ten mien"
  // im lang that bai thi khong co cach nao biet board dang hoi ai - phai di doan tung gia
  // thuyet mot. In ra day, ngay sau khi chot xong duong mang, la re nhat va dung luc nhat.
  if (netConnected()) {
    IPAddress d1 = eth_connected ? ETH.dnsIP(0) : WiFi.dnsIP(0);
    IPAddress d2 = eth_connected ? ETH.dnsIP(1) : WiFi.dnsIP(1);
    LOG("DNS: %s / %s (qua %s)", d1.toString().c_str(), d2.toString().c_str(), netLinkName());
    if ((uint32_t)d1 == 0) {
      LOG("DNS: bang DNS RONG - hostByName() se that bai NGAY, khong gui goi nao. "
          "URL dung ten mien se im lang khong chay; dung IP thi van duoc.");
    }
  }

  // Ten AP la thu duy nhat operator doc duoc tu dien thoai khi khong vao duoc Web UI, nen no
  // phai noi dung su that - diagApName() lo viec chon tag. "STATIC-192.168.99.5" tren mot board
  // khong cam day mang la mot loi noi doi rat dat: nguoi ta se di tim IP do tren mang thay vi
  // di kiem tra soi day.
  //
  // Phat CA khi khong co mang (tag "NOLINK"): vua la duong vao Web UI cuoi cung con lai (noi
  // vao AP roi mo http://192.168.4.1) de con sua duoc SSID/IP tinh ma khong phai cam laptop +
  // Serial vao tan noi, vua la cach phan biet tu xa giua "board chay nhung mat mang" voi "board
  // mat dien" (khong thay AP nao ca).
  if (!netConnected()) {
    LOG("Khong co IP tren ca ETH lan WiFi - phat diag AP de con vao Web UI qua 192.168.4.1");
  }
  ethUsedFallback = usedFallback;
  startDiagAp();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/test_iot", HTTP_POST, handleTestIot);
  server.on("/test_relay", HTTP_POST, handleTestRelay);
  server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);
  server.on("/update_url", HTTP_POST, handleUpdateUrl);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.begin();

  mqttInit();
  LOG("HTTP Server Started");
}

// ======================================================================
// NET WATCHDOG - mat ETH qua lau thi tu reboot de di lai luong boot (cho ETH 1 phut -> WiFi)
// ======================================================================
// Ly do ton tai: wifiTryConnect() chi chay trong setup(), nen mat ETH GIUA CHUNG khong tu keo
// WiFi len duoc - board se nam im khong mang cho toi khi co nguoi toi rut nguon. Reboot la cach
// re nhat de dung lai chinh cai luong da co ma khong phai xu ly hai netif song song, MQTT bam
// netif nao, diag AP da tat thi bat lai kieu gi.
//
// Gia phai tra: relay + sensor ngung vai giay moi lan reboot. Vi the co 6 chot chan de no chi
// no ra khi that su co loi va that su co cho de lui ve:
static void netWatchdogTick() {
  const unsigned long NET_LOST_REBOOT_MS = 60000UL;  // mat ETH lien tuc bao lau thi reboot
  const unsigned long NET_OK_CLEAR_MS    = 60000UL;  // mang lanh lien tuc bao lau thi xoa bo dem
  const uint32_t NET_REBOOT_MAX = 1;                 // toi da bao nhieu lan reboot lien tiep

  static unsigned long netLostSince = 0;
  static unsigned long netOkSince = 0;
  static bool loggedNeverUp = false;
  static bool loggedNoWifi = false;
  static bool loggedGaveUp = false;

  // (1) Dang chay bang WiFi du phong: ETH tat la trang thai BINH THUONG o day, khong phai su
  // co. Reboot khong dua ta di dau ngoai viec cat mang cua chinh minh them 20 giay.
  if (wifiFallbackActive) return;

  // (2) ETH CHUA BAO GIO len trong phien nay: watchdog sinh ra de cuu tinh huong "dang chay thi
  // mat mang". Con neu boot len da khong co ETH thi setup() vua chay tron ven chuoi cho-ETH-1-
  // phut -> thu-WiFi roi, reboot lai chi la lam y het them lan nua, khong co gi moi - doi lai
  // thi cat mat diag AP dung luc no la duong vao duy nhat con lai de sua cau hinh.
  if (!ethEverUp) {
    if (!loggedNeverUp) {
      LOG("Net watchdog: ETH chua len lan nao trong phien nay - watchdog dung yen (setup() da thu du ETH -> WiFi)");
      loggedNeverUp = true;
    }
    return;
  }

  // (3) Co may DANG NOI vao diag AP: nhieu kha nang operator dang go lai SSID/IP tinh ngay luc
  // nay - reboot giua chung la pha hoai. Chi hoan khi that su co nguoi noi vao, KHONG hoan chi
  // vi AP dang bat: AP phat 5 phut moi lan boot, lay "AP bat" lam dieu kien thi watchdog cam
  // suot 5 phut dau ke ca khi khong ai dung toi no.
  if (diagApActive && WiFi.softAPgetStationNum() > 0) return;

  if (eth_connected) {
    netLostSince = 0;
    if (netOkSince == 0) {
      netOkSince = millis();
    } else if (netRebootCount > 0 && (millis() - netOkSince) >= NET_OK_CLEAR_MS) {
      // Mang da lanh lai va tru duoc 1 phut - coi nhu su co truoc do da qua, tra bo dem ve 0 de
      // lan hong SAU van con quyen reboot. Khong co dong nay thi mot su co duy nhat luc 9 gio
      // sang lam tat watchdog cho het ngay.
      LOG("Net watchdog: ETH on dinh tro lai - xoa bo dem reboot");
      netRebootCount = 0;
      ethReturnRebootCount = 0;
      loggedNoWifi = false;
      loggedGaveUp = false;
    }
    return;
  }

  netOkSince = 0;

  if (netLostSince == 0) {
    netLostSince = millis();
    LOG("Net watchdog: mat ETH - se tu reboot neu khong co lai trong %lu giay", NET_LOST_REBOOT_MS / 1000UL);
    return;
  }
  if ((millis() - netLostSince) < NET_LOST_REBOOT_MS) return;

  // (4) Dang tai firmware tu URL: reboot vao giua se de lai mot ban firmware ghi do dang.
  if (otaUrlPending) return;

  // (5) Firmware nay CHUA duoc xac nhan (vua OTA xong, dang trong cua so chay thu): mot cu
  // reset o day se bi bootloader hieu la "ban moi that bai" va LUI VE BAN CU. Tuc watchdog se
  // am tham huy dung cai firmware vua nap - va lap lai y het o moi lan OTA sau. Cho xac nhan
  // xong roi reboot; cham nhat la doi them 60 giay.
  if (otaPendingVerify()) return;

  // (6) Khong cau hinh WiFi du phong thi reboot la vo nghia: boot lai cung chi ra dung trang
  // thai khong mang nay, doi lai relay ngung 20 giay moi 2 phut. Tha nam im con hon.
  if (!wifiEnabled || wifiSsid[0] == '\0') {
    if (!loggedNoWifi) {
      LOG("Net watchdog: mat ETH nhung chua cau hinh WiFi du phong - khong reboot (reboot cung khong co cho de lui ve)");
      loggedNoWifi = true;
    }
    return;
  }

  if (netRebootCount >= NET_REBOOT_MAX) {
    if (!loggedGaveUp) {
      LOG("Net watchdog: da reboot %u lan ma van khong co mang - thoi, khong reboot nua (relay/sensor van chay). Cup nguon de dat lai bo dem.", (unsigned)netRebootCount);
      loggedGaveUp = true;
    }
    return;
  }

  netRebootCount++;
  LOG(">>> Net watchdog: mat ETH qua %lu giay - reboot lan %u de thu WiFi du phong <<<",
      NET_LOST_REBOOT_MS / 1000UL, (unsigned)netRebootCount);
  delay(200); // cho dong log tren ra het khoi UART truoc khi cat dien
  ESP.restart();
}

// ======================================================================
// ETH QUAY LAI trong luc dang chay WiFi du phong -> reboot de ve lai ETH (dung IP tinh)
// ======================================================================
// Neu khong co ham nay: luc boot khong co link thi ca khoi ETH.config() lan khoi cho DHCP deu
// bi bo qua, nen netif ETH van nam o che do DHCP MAC DINH. Cam lai day giua chung la no lang le
// xin DHCP va bat len bang mot IP la - dung luc operator dang mong doi dung cai IP tinh da cau
// hinh. Te hon nua la luc do hai netif cung song voi hai dia chi khac nhau, ETH lai co do uu
// tien dinh tuyen cao hon WiFi, nen luong ra ngoai am tham doi duong ma khong bao gi.
//
// Sua bang cach ap IP tinh cho ETH ngay tu luc boot (khi chua co link) thi KHONG duoc: WiFi du
// phong dang giu dung dia chi do roi, hai netif cung mot IP tren cung mot lop 2 = xung dot ARP.
// Reboot la cach sach nhat: setup() chay lai voi day da cam san, di dung nhanh uu tien IP tinh,
// va WiFi khong bao gio duoc bat len.
static void ethReturnTick() {
  const unsigned long ETH_LINK_STABLE_MS = 15000UL; // link phai on dinh bao lau moi tinh la "da cam lai"
  const uint32_t ETH_RETURN_REBOOT_MAX = 2;

  static unsigned long linkUpSince = 0;
  static bool loggedGaveUp = false;

  // Chi co y nghia khi setup() KET THUC MA ETH KHONG LEN - bao gom ca hai nhanh: dang chay WiFi
  // du phong, VA dang chay khong co mang nao ca (NOLINK). Truoc day dieu kien la
  // "wifiFallbackActive" nen bo sot han nhanh NOLINK: board boot khi chua cam day va WiFi cung
  // hong, sau do cam day vao thi khong ai phat hien, netif ETH lang le xin DHCP va len bang mot
  // IP la - dung cai ma cau hinh "uu tien IP tinh" noi rang no se khong lam.
  if (ethUpAtBoot) return;
  if (otaUrlPending) return;

  // Nhu chot (5) cua netWatchdogTick(): reboot trong cua so chay thu = bootloader lui ve ban cu,
  // tuc tu huy chinh firmware vua nap.
  if (otaPendingVerify()) return;

  if (!ethLinkPresent) {
    linkUpSince = 0;
    return;
  }

  // Doi link on dinh thay vi reboot ngay khi thay link: day mang long chan hay chop tat lien
  // tuc, phan ung tuc thi se thanh mot chuoi reboot lien mien.
  if (linkUpSince == 0) {
    linkUpSince = millis();
    LOG("ETH co link tro lai - neu on dinh %lu giay se reboot de quay ve ETH voi IP tinh",
        ETH_LINK_STABLE_MS / 1000UL);
    return;
  }
  if ((millis() - linkUpSince) < ETH_LINK_STABLE_MS) return;

  // Giong netWatchdogTick(): dung cat ngang luc co nguoi dang cau hinh qua diag AP.
  if (diagApActive && WiFi.softAPgetStationNum() > 0) return;

  if (ethReturnRebootCount >= ETH_RETURN_REBOOT_MAX) {
    if (!loggedGaveUp) {
      LOG("ETH quay lai lan thu %u ma van khong tru duoc - thoi, o lai WiFi (cup nguon de dat lai bo dem)",
          (unsigned)ethReturnRebootCount);
      loggedGaveUp = true;
    }
    return;
  }

  ethReturnRebootCount++;
  LOG(">>> ETH da cam lai va on dinh - reboot de quay ve ETH voi IP tinh (lan %u) <<<",
      (unsigned)ethReturnRebootCount);
  delay(200); // cho dong log ra het khoi UART truoc khi cat dien
  ESP.restart();
}

void loop() {
  // "|| diagApActive": khi ca ETH lan WiFi deu chet, diag AP la duong duy nhat vao Web UI
  // (http://192.168.4.1) - khong phuc vu HTTP o day thi cai AP do vo dung.
  if (netConnected() || diagApActive) {
    server.handleClient();
  }

  diagApTick();

  updateTestSequence();
  checkSensors();

  // Cap nhat o DAY chu khong trong netWatchdogTick(): ca hai ham duoi deu doc co nay, de viec
  // gan nam trong mot ham roi ham kia doc ke thi thu tu goi tro thanh mot rang buoc ngam.
  if (eth_connected) ethEverUp = true;
  wifiRetryTick();
  netWatchdogTick();
  ethReturnTick();

  // Khoi tao bang millis() (chay dung 1 lan) chu khong phai 0: uptime luc vao loop() da ~12s
  // (cho Serial + cho DHCP), nen voi chu ky heartbeat ngan (min 5s) moc 0 se lam nhip dau ban
  // ngay o vong loop dau tien. Dat sau checkSensors() de lan gui dau tien chac chan da co
  // trang thai sensor that, khong phai gia tri khoi tao toan FALSE (=TRONG).
  static unsigned long lastHeartbeatMs = millis();
  if (heartbeatInterval > 0 && (millis() - lastHeartbeatMs) >= heartbeatInterval) {
    lastHeartbeatMs = millis();
    resyncBookState();
  }

  otaRollbackTick();

  otaUrlTick();  // CUOI loop: no chan ~10-30s roi reboot, dat truoc thi cac buoc tren bi treo theo
}
