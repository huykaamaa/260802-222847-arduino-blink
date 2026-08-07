#pragma once

void handleData();
void handleSave();
// MOT route duy nhat cho ca 2 kenh: chuoi test goi triggerBookState(), von ban ca MQTT lan
// OSC, nen 2 handler rieng truoc day co than ham y het nhau.
void handleTestIot();
void handleTestRelay();

// OTA qua web: handleUpdateUpload() nhan tung chunk file trong luc dang upload (dang ky lam
// callback thu 2 cua server.on("/update", ...)), handleUpdateFinish() chay SAU KHI upload
// xong (chunk cuoi), tra ket qua va tu reboot neu OK.
void handleUpdateUpload();
void handleUpdateFinish();

// Reset mem board qua Web UI (ESP.restart()). Gated giong cac route doi trang thai khac.
void handleReboot();

// Tick tung buoc cua chuoi Test (goi tu loop() trong main.cpp) - khong block.
void updateTestSequence();

void loadConfig();
int saveConfig();
