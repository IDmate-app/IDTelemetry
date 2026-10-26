#include "mod_ble_scan.h"
#include "mod_elm327.h"
#include "mod_logs.h"
#include <NimBLEDevice.h>

// ============================================================
//  mod_ble_scan - BLE Server (NimBLE Init + Server-Instanz)
// ============================================================

static NimBLEServer* pServer = nullptr;

void ble_scan_init() {
    NimBLEDevice::init(ELM327_BLE_NAME);  // "OBDII" — ABRP/Torque suchen danach
    NimBLEDevice::setMTU(185);            // Größere Pakete erlauben (statt 23 default)
    pServer = NimBLEDevice::createServer();
    syslog("BLE", "NimBLE bereit");
}

NimBLEServer* ble_get_server() { return pServer; }
