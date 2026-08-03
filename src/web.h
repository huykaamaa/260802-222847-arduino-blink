#pragma once

void handleData();
void handleSave();
void handleTestMQTT();
void handleTestOSC();
void handleTestRelay();

// Tick tung buoc cua chuoi Test (goi tu loop() trong main.cpp) - khong block.
void updateTestSequence();

void loadConfig();
int saveConfig();
