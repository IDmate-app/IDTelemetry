#pragma once
#include "shared.h"
#include <NimBLEDevice.h>

// ============================================================
//  mod_ble_scan - BLE Server (NimBLE Init + Server-Instanz)
// ============================================================

void          ble_scan_init();
NimBLEServer* ble_get_server();  // gibt den BLE Server zurück für andere Module
