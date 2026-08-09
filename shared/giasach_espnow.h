#pragma once

/*
 * Giao thuc ESP-NOW dung CHUNG giua node phu (doc sensor, o xa) va node chinh (W5500 +
 * relay + MQTT/OSC). File nay duoc include boi CA HAI project:
 *   - node chinh : gia_sach/platformio.ini      -> build_flags = -I shared
 *   - node phu   : gia_sach/node_sensor/...     -> build_flags = -I ../shared
 * Sua struct o day thi PHAI nap lai firmware CA HAI board. GIASACH_PROTO_VERSION o duoi
 * chinh la chot chan: ben nhan bo qua goi khac version, khong doc nham byte.
 */

#include <stdint.h>

// "GSN" + so. Doi magic neu sau nay chay 2 he thong gia sach gan nhau va khong muon chung
// nghe nham cua nhau (kem theo doi ca PMK/LMK trong node_config.h).
#define GIASACH_MAGIC   0x47534E31UL
#define GIASACH_PROTO_VERSION 1

// ----------------------------------------------------------------------
// Loai goi
// ----------------------------------------------------------------------
#define GIASACH_MSG_STATE 0x01  // node phu -> node chinh: trang thai sensor + nonce
#define GIASACH_MSG_CMD   0x02  // node chinh -> node phu: lenh (reboot/ping)

// Lenh trong GIASACH_MSG_CMD
#define GIASACH_CMD_PING   0x01  // yeu cau node phu tra loi ngay 1 goi STATE (do link)
#define GIASACH_CMD_REBOOT 0x02  // reset node phu (esp_restart)

// Co trong GiaSachStateMsg.flags
#define GIASACH_FLAG_BOOT     0x01  // goi dau tien sau khi node phu khoi dong
#define GIASACH_FLAG_PONG     0x02  // goi nay la tra loi cho CMD_PING
#define GIASACH_FLAG_SETTLING 0x04  // debounce chua on dinh xong sau boot, tri so tam thoi

// ----------------------------------------------------------------------
// Trang thai sach - PHAI trung voi BOOK_STATE_* trong src/globals.h cua node chinh
// ----------------------------------------------------------------------
#define GIASACH_STATE_UNKNOWN   0
#define GIASACH_STATE_FULL      1
#define GIASACH_STATE_ONE_TAKEN 2
#define GIASACH_STATE_TWO_TAKEN 3

// ----------------------------------------------------------------------
// Goi trang thai: node phu -> node chinh
// ----------------------------------------------------------------------
// Debounce lam O NODE PHU, node chinh chi nhan ket qua da sach. Van gui ca sensor1/sensor2
// tho (da debounce) de Web UI cua node chinh hien duoc tung vi tri chu khong chi bookState.
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  version;
  uint8_t  type;       // GIASACH_MSG_STATE
  uint16_t seq;        // tang moi goi, wrap 16 bit - de node chinh bo goi lap/den tre
  uint8_t  sensor1;    // 1 = co sach o vi tri 1
  uint8_t  sensor2;    // 1 = co sach o vi tri 2
  uint8_t  bookState;  // GIASACH_STATE_*
  uint8_t  flags;      // GIASACH_FLAG_*
  uint32_t uptimeMs;   // uptime node phu, de biet no vua reset hay chay lien tuc
  uint32_t nonce;      // thu thach cho lenh ke tiep - xem giaSachCmdToken() ben duoi
} GiaSachStateMsg;

// ----------------------------------------------------------------------
// Goi lenh: node chinh -> node phu
// ----------------------------------------------------------------------
// Kenh ESP-NOW da duoc ma hoa bang PMK/LMK, nhung REBOOT la lenh pha hoai neu bi phat lai
// (replay), nen them lop nonce: node phu phat 1 nonce ngau nhien trong moi goi STATE, node
// chinh phai tra dung token tinh tu nonce DO. Nhan xong 1 lenh hop le thi node phu doi
// nonce ngay -> moi lenh chi dung duoc dung 1 lan.
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  version;
  uint8_t  type;       // GIASACH_MSG_CMD
  uint8_t  cmd;        // GIASACH_CMD_*
  uint8_t  reserved;   // giu 0, de danh cho tham so lenh sau nay
  uint32_t nonce;      // phai trung nonce moi nhat node phu phat ra
  uint32_t token;      // giaSachCmdToken(nonce, cmd, secret)
} GiaSachCmdMsg;

// ----------------------------------------------------------------------
// Token lenh
// ----------------------------------------------------------------------
// FNV-1a 32 bit tren (nonce | cmd | secret). Khong phai MAC mat ma that su - lop bao mat
// chinh la ma hoa ESP-NOW (PMK/LMK); cai nay chi de chan replay va chan lenh bua tu thiet bi
// khong biet secret. Dung ham inline chung de hai ben khong bao gio tinh lech nhau.
static inline uint32_t giaSachCmdToken(uint32_t nonce, uint8_t cmd, const char* secret) {
  uint32_t h = 2166136261UL;
  uint8_t buf[5];
  buf[0] = (uint8_t)(nonce & 0xFF);
  buf[1] = (uint8_t)((nonce >> 8) & 0xFF);
  buf[2] = (uint8_t)((nonce >> 16) & 0xFF);
  buf[3] = (uint8_t)((nonce >> 24) & 0xFF);
  buf[4] = cmd;
  for (uint8_t i = 0; i < sizeof(buf); i++) {
    h ^= buf[i];
    h *= 16777619UL;
  }
  for (const char* p = secret; p && *p; p++) {
    h ^= (uint8_t)(*p);
    h *= 16777619UL;
  }
  return h;
}
