#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <ETH.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mqtt_client.h>

#define LOG(fmt, ...) do { Serial.printf(fmt "\r\n", ##__VA_ARGS__); } while (0)

// --- NVS / Preferences key-length guard --------------------------------------------------
// ESP32 NVS caps key names at 15 usable chars + NUL. Wrapping every NVS key literal in
// NVS_KEY() fails the BUILD instead of silently losing data if a key is ever renamed past
// that limit.
template <size_t N>
constexpr const char* NVS_KEY(const char (&s)[N]) {
  static_assert(N <= 16, "NVS key too long: max 15 chars + NUL");
  return s;
}

// ======================================================================
// PIN CONFIG
// ======================================================================

// ETH (W5500 over SPI)
#define ETH_CS   10
#define ETH_MOSI 11
#define ETH_MISO 13
#define ETH_SCK  12
#define ETH_INT  -1
#define ETH_RST  -1

// Chi con 2 sensor (vi tri 1+2), logic GOP: xem checkSensors() trong main.cpp.
#define SENSOR_NUM 2
extern const uint8_t sensorPins[SENSOR_NUM];

// Van con du 6 relay vat ly (day dien san co tu truoc) - dung 3 CAP relay de BACKUP: cap nao
// duoc tick (relayPairEnable) thi nhan cung tin hieu relay1 (luon ON) / relay2 (theo sach), cap nao
// khong tick thi luon OFF. Tick nhieu cap cung luc = chay song song du phong; hong 1 cap thi bo
// tick cap do, tick cap con lai, khong can nap lai firmware.
#define RELAY_NUM 6
#define RELAY_PAIR_NUM (RELAY_NUM / 2)
extern const uint8_t relayPins[RELAY_NUM];
extern bool relayPairEnable[RELAY_PAIR_NUM];

#define SENSOR_ACTIVE LOW
#define RELAY_ON  HIGH
#define RELAY_OFF LOW
#define RELAY_TEST_PULSE_MS 2000UL

// Trang thai sach: 1 = du sach (ca 2 sensor kich), 2 = da lay 1 cuon (dung 1 trong 2 sensor
// kich), 3 = da lay ca 2 cuon (khong sensor nao kich).
#define BOOK_STATE_FULL      1
#define BOOK_STATE_ONE_TAKEN 2
#define BOOK_STATE_TWO_TAKEN 3

// Local UDP port de mo socket OSC (chi gui di, khong nghe). Tach ra hang so de khong bi
// nham voi oscPort (port DICH, cau hinh duoc tren web).
#define OSC_LOCAL_PORT 9000

// ======================================================================
// STATE / CONFIG - dinh nghia trong main.cpp
// ======================================================================
extern SPIClass spi;
extern WebServer server;
extern Preferences prefs;
extern WiFiUDP oscUdp;
extern bool eth_connected;
extern bool wifi_connected;

// Duong mang nao dang thuc su cam - dung cho loop() (co chay web server khong), cho SSID cua
// diag AP va cho dashboard. Dinh nghia trong main.cpp.
bool netConnected();
IPAddress netLocalIP();
const char* netLinkName();   // "ETH" / "WiFi" / "-"

// netConnected() chi noi "co IP gan tren netif" - van true khi bo IP tinh duoc ap vao mot cong
// KHONG cam day. netVerified() moi la "co bang chung noi duoc voi mang" (DHCP cap IP, hoac
// gateway tra loi ping). Dashboard phai dung cai thu hai.
bool netVerified();

// So lan board da TU reboot vi mat ETH trong phien cam dien nay (xem netWatchdogTick() trong
// main.cpp). Hien len dashboard vi mot cu reboot giua buoi dien la vo hinh voi operator - khong
// co so nay thi ho chi thay "sao no lai vao trang thai khoi dong?" ma khong biet tai dau.
uint32_t netLossReboots();     // reboot vi MAT ETH giua chung
uint32_t ethReturnReboots();   // reboot vi CAM LAI day ETH (de quay ve IP tinh)

extern bool diagApActive;
extern unsigned long diagApStartMs;

// MQTT
extern esp_mqtt_client_handle_t mqtt;
extern bool mqttConnected;
extern bool mqttEnabled;
extern char mqttServer[32];
extern uint16_t mqttPort;
extern char mqttUser[32];
extern char mqttPass[32];

// Moi trang thai (FULL/ONE_TAKEN/TWO_TAKEN) co topic + payload rieng, ban 1 lan khi VUA
// CHUYEN vao trang thai do (edge-triggered, giong OSC ben duoi).
extern char mqttTopicFull[64];
extern char mqttValueFull[32];
extern char mqttTopicOneTaken[64];
extern char mqttValueOneTaken[32];
extern char mqttTopicTwoTaken[64];
extern char mqttValueTwoTaken[32];

// OSC - 3 trang thai = 3 message OSC hoan toan tach biet (dia chi rieng + int rieng moi cai).
extern bool oscEnabled;
extern char oscIp[32];
extern uint16_t oscPort;
extern char oscAddressFull[64];
extern int oscValueFull;
extern char oscAddressOneTaken[64];
extern int oscValueOneTaken;
extern char oscAddressTwoTaken[64];
extern int oscValueTwoTaken;

// Admin auth (Basic Auth tren cac endpoint thay doi trang thai)
extern char authUser[32];
extern char authPass[32];

// --- Ma nhan dang ban firmware dang chay (2026-08-10, port tu can_tim) -------------------
// Tinh tu CHINH anh da nap (ESP.getSketchMD5()), khong phai tu macro __DATE__/__TIME__.
// Ly do: PlatformIO chi bien dich lai file nao thay doi, nen dau thoi gian compile nam trong
// web.cpp se giu nguyen gia tri CU neu lan build do chi sua html.cpp - tuc no sai dung luc can
// no nhat, la luc kiem tra xem OTA da that su doi firmware chua. MD5 doc tu flash thi khong
// bao gio noi doi: byte khac nhau la ma khac nhau.
//
// Tinh 1 lan trong setup() roi cache: getSketchMD5() phai doc het ~1.2MB flash, khong the goi
// moi lan /data (dashboard poll lien tuc).
extern char fwId[9];      // 8 ky tu dau cua MD5 sketch
extern uint32_t fwSize;   // kich thuoc sketch (byte)
void fwIdInit();

// --- OTA tu URL (2026-08-10, port tu can_tim) --------------------------------------------
// Thay vi chon file upload qua form, board TU TAI firmware.bin ve tu mot URL da luu san (NVS
// key "ota_url") roi tu ghi flash va reboot. Tien khi phai nap nhieu board: bat mot HTTP server
// trong LAN, moi board chi con mot nut bam.
//
// Ca 3 phong dung chung 1 server nhung KHAC TEN FILE (cantim.bin / giasach.bin / datthe.bin) -
// xem tools/copy_fw.py, no tu copy sau moi lan build. Phong nay dung giasach.bin.
//
// CHI HO TRO http:// - https:// can NetworkClientSecure kem chung chi, ton them ~100KB flash va
// them may kieu loi kho doan; trong mang show khep kin thi khong dang. handleUpdateUrl() TU CHOI
// thang https:// ngay luc luu, thay vi de no that bai luc dang tai.
//
// BAO MAT: ai kiem soat duoc URL nay thi kiem soat duoc firmware cua board. Chi tro vao may
// trong mang noi bo, dung tro ra Internet qua HTTP tran.
extern char otaUrl[96];

// --- OTA rollback guard (2026-08-13) ------------------------------------------------------
// Bootloader cua ban ESP-IDF nay CO san co che rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
// =y): anh vua OTA duoc danh dau PENDING_VERIFY, neu thiet bi reset ma chua ai xac nhan la
// "chay tot" thi lan boot sau bootloader tu quay ve anh cu.
//
// Nhung Arduino vo hieu hoa no: initArduino() goi thang esp_ota_mark_app_valid_cancel_rollback()
// TRUOC khi setup() chay (xem esp32-hal-misc.c). Tuc anh moi duoc cong nhan la tot vai mili
// giay truoc khi no kip chung minh dieu do - dung mot ban firmware chet ngay trong setup() thi
// mang luoi an toan nay khong bat duoc gi ca. Da xay ra that: mot ban loi OTA cho board o xa,
// chet o setup(), va vi chua kip len mang nen khong con duong nao vao ngoai cam USB tan noi.
//
// Cach chua: ghi de weak symbol verifyRollbackLater() cho no tra ve true (hoan xac nhan), roi
// TU xac nhan trong loop() sau khi da chay lien tuc OTA_CONFIRM_MS. Firmware chet truoc moc do
// => lan boot ke tiep bootloader tu lui ve ban cu dang chay tot.
//
// Danh doi phai biet: trong cua so cho xac nhan, CUP NGUON cung bi tinh la "khong qua duoc bai
// kiem tra" va se lui ve ban cu - du firmware moi hoan toan binh thuong. Vi the cua so de ngan
// (xem OTA_CONFIRM_MS trong main.cpp) va dashboard co bao dang o trang thai chay thu.
void otaRollbackInit();
void otaRollbackTick();
bool otaPendingVerify();     // true khi ban dang chay VAN CHUA duoc xac nhan
uint32_t otaConfirmRemainSec();  // con bao nhieu giay nua thi xac nhan (0 neu khong con cho)

// Dat true tu handleUpdateUrl(); otaUrlTick() trong loop() moi thuc su tai. KHONG goi
// httpUpdate.update() thang trong handler: no chan 10-30 giay roi reboot giua chung, response
// chua kip ra khoi socket nen trinh duyet bao loi mang du update chay dung.
extern bool otaUrlPending;
void otaUrlTick();

// ETH static-IP: mac dinh la fallback (chi dung khi DHCP khong len duoc). Neu
// ethUseStaticFirst=true thi dung IP tinh ngay tu dau, bo qua cho DHCP (10s) hoan toan;
// van tu dong lui ve DHCP neu IP tinh nhap sai/khong parse duoc.
extern char ethStaticIp[16];
extern char ethStaticGateway[16];
extern char ethStaticNetmask[16];
extern bool ethUseStaticFirst;

// --- WiFi du phong (2026-08-13) -----------------------------------------------------------
// ETH van la duong CHINH: luc boot board cho toi ETH_TOTAL_WAIT_MS (1 phut) de Ethernet len
// duoc IP. Chi khi het phut do ma van khong co gi (thuong la khong cam day / switch chet) thi
// moi bat WiFi STA de it nhat con vao duoc Web UI + ban MQTT/OSC.
//
// Dung CHUNG bo IP tinh voi ETH (ethStaticIp/Gateway/Netmask) khi ethUseStaticFirst = true -
// operator chi phai nho MOT dia chi cho mot board du no dang cam day hay dang chay wifi. An
// toan vi hai duong nay khong bao gio song cung luc: chi vao nhanh WiFi khi ETH da that bai.
// Bo tick uu tien IP tinh thi WiFi xin DHCP nhu binh thuong.
//
// KHONG tu chuyen nguoc ve ETH khi cam lai day giua chung - phai reboot. Chuyen qua lai giua 2
// netif luc dang chay se lam MQTT/OSC dut quang kho doan hon la mot lan reboot chu dong.
extern bool wifiEnabled;
extern char wifiSsid[33];   // chuan 802.11: toi da 32 ky tu + NUL
extern char wifiPass[64];   // WPA2-PSK: toi da 63 ky tu + NUL; de trong = mang mo

// Debounce dung chung cho ca 2 sensor
extern unsigned long debounceTime;

// Heartbeat/resync: dinh ky gui lai trang thai sach hien tai qua MQTT/OSC (khong doi
// topic/dia chi, chi gui lai gia tri hien tai) - de bu lai neu message tai thoi diem doi
// trang thai bi rot mang (MQTT QoS0/UDP OSC deu khong dam bao gui toi noi). 0 = tat heartbeat.
extern unsigned long heartbeatInterval;

// Per-sensor state (debounce), tach biet voi relay dau ra (relay la KET QUA cua logic gop
// ca 2 sensor, khong con 1:1 voi tung sensor nua - xem checkSensors() trong main.cpp).
extern bool sensorState[SENSOR_NUM];   // trang thai da debounce: true = co sach o vi tri do
extern bool relayOutput[RELAY_NUM];    // trang thai THUC TE tung relay vat ly (0..5), hien len web
extern int bookState;                  // BOOK_STATE_FULL / _ONE_TAKEN / _TWO_TAKEN, 0 = chua xac dinh (truoc lan check dau)

// Nut Test tren web: dao trang thai CA 2 relay tam thoi (~2s) de kiem tra day dien, bat ke
// trang thai sach thuc te. checkSensors() tu dong tra ve dung trang thai (tinh theo bookState)
// ngay khi xung Test ket thuc - khong can goi resync rieng, vi relay duoc tinh lai moi vong
// loop() thay vi edge-triggered nhu MQTT/OSC. Dinh nghia trong main.cpp.
void triggerRelayTest();
bool relayTestActive();

// Gui lai trang thai sach HIEN TAI qua MQTT/OSC, dung nguyen topic/dia chi/gia tri binh
// thuong - chi la "nhac lai" cue gan nhat, khong phai message rieng. Dung cho heartbeat dinh
// ky va cho buoc ket thuc chuoi Test. Dinh nghia trong main.cpp.
void resyncBookState();
