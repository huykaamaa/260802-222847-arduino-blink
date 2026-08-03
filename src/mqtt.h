#pragma once

#include <esp_event.h>

void mqttInit();
void mqttEvent(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

// Bao trang thai sach hien tai (BOOK_STATE_*) qua MQTT + OSC (goi tu checkSensors() trong
// main.cpp va tu chuoi Test trong web.cpp).
void triggerBookState(int state);
