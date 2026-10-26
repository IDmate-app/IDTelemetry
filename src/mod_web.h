#pragma once
void ws_broadcast_json(const char* json);
#include "shared.h"

// ============================================================
//  mod_web - WiFi AP + AsyncWebServer + WebSocket
// ============================================================

void web_init();
void web_ap_update(); // im loop() aufrufen — AP-Timeout + VBUS-Restart
void web_ap_stop();   // AP + WebServer sofort abschalten
void web_ap_start();  // AP + WebServer einschalten (2-min-Timer neu)
bool web_ap_active(); // true solange AP noch läuft

void ble_web_routes_init();