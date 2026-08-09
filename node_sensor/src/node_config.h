#pragma once

/*
 * Cau hinh node PHU. Cac gia tri o day phai KHOP voi node chinh:
 *   ESPNOW_CHANNEL, ESPNOW_PMK, ESPNOW_LMK, GIASACH_CMD_SECRET
 * Lech mot cai thoi la hai board khong noi duoc nhau (va khong bao loi ro rang, chi im lang
 * khong nhan goi) - nen sua thi sua ca hai roi nap lai ca hai.
 */

#include <stdint.h>

// ======================================================================
// DIA CHI NODE CHINH
// ======================================================================
// MAC WiFi station cua board chinh (KHONG phai MAC Ethernet W5500).
// Cach lay: nap firmware node chinh, mo Serial - se in "ESP-NOW: MAC node chinh = ...".
// Truoc khi co dong do thi chay tam WiFi.macAddress() tren board chinh de doc.
// Sai MAC = node phu gui vao hu vo, Serial se bao "send FAIL" lien tuc.
static const uint8_t MAIN_NODE_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

// ======================================================================
// RADIO
// ======================================================================
// Kenh WiFi dung cho ESP-NOW. Ca hai board phai cung kenh. Neu node chinh con phat diag AP
// thi AP do cung phai o kenh nay. Chon kenh it dung o hien truong (1 / 6 / 11).
#define ESPNOW_CHANNEL 1

// Long-range mode cua Espressif: tang tam ~2-3 lan nhung phai bat o CA HAI dau.
// 50 m thoang cung tang thi ESP-NOW thuong da du -> de 0. Chi bat khi do thuc te thay rot goi.
#define ESPNOW_LONG_RANGE 0

// Khoa ma hoa ESP-NOW. PMK va LMK deu PHAI dung 16 ky tu (khong hon khong kem).
// DOI hai chuoi nay truoc khi lap dat that - de nguyen mac dinh nghia la ai doc repo cung
// giai ma duoc va gui duoc lenh reboot.
#define ESPNOW_PMK "GiaSachPMK__2026"
#define ESPNOW_LMK "GiaSachLMK__2026"

// Secret cho token lenh (chan replay lenh reboot) - xem giaSachCmdToken() trong
// shared/giasach_espnow.h. Do dai tuy y, cung phai DOI truoc khi lap dat that.
#define GIASACH_CMD_SECRET "doi-chuoi-nay-truoc-khi-lap-dat"

// ======================================================================
// SENSOR (TCRT5000, giong node chinh: DO keo xuong LOW khi phat hien vat)
// ======================================================================
#define NODE_SENSOR_NUM 2
static const uint8_t nodeSensorPins[NODE_SENSOR_NUM] = { 1, 2 };
#define SENSOR_ACTIVE LOW

// Debounce (ms) - lam o day chu khong o node chinh, de duong truyen chi mang trang thai da
// sach. Giu bang mac dinh cu cua node chinh (500 ms).
#define NODE_DEBOUNCE_MS 500

// ======================================================================
// NHIP GUI
// ======================================================================
// Heartbeat: gui lai trang thai hien tai deu dan ke ca khi khong doi gi. Node chinh dua vao
// nhip nay de biet node phu con song (mat NODE_LINK_TIMEOUT_MS thi bao mat link).
#define NODE_HEARTBEAT_MS 300

// Khi trang thai DOI: ban 1 chum nhieu goi cach nhau NODE_BURST_GAP_MS de mot goi rot mang
// khong lam tre cue tan den nhip heartbeat sau. ESP-NOW co ACK nhung khong tu gui lai.
#define NODE_BURST_COUNT 3
#define NODE_BURST_GAP_MS 25

// Bo qua NODE_SETTLE_MS dau sau boot: trong khoang nay van gui (de node chinh biet node phu
// da song) nhung gan co GIASACH_FLAG_SETTLING, vi debounce chua chay du mot chu ky nen tri
// so co the chua dung.
#define NODE_SETTLE_MS 1500

// ======================================================================
// TU PHUC HOI
// ======================================================================
// Neu da tung gui thanh cong ROI sau do mat trang nay lau, coi nhu radio treo -> tu reset.
// Chi tinh SAU lan gui thanh cong dau tien: luc moi boot ma node chinh chua len thi node phu
// khong duoc phep tu reboot vong lap.
#define NODE_RADIO_STUCK_MS 120000UL
