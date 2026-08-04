#pragma once

void handleData();
void handleSave();
void handleTestMQTT();
void handleTestOSC();
void handleTestRelay();

// OTA qua web: handleUpdateUpload() nhan tung chunk file trong luc dang upload (dang ky lam
// callback thu 2 cua server.on("/update", ...)), handleUpdateFinish() chay SAU KHI upload
// xong (chunk cuoi), tra ket qua va tu reboot neu OK.
void handleUpdateUpload();
void handleUpdateFinish();

// Tick tung buoc cua chuoi Test (goi tu loop() trong main.cpp) - khong block.
void updateTestSequence();

void loadConfig();
int saveConfig();
