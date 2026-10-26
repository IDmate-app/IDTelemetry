#pragma once
#include "shared.h"

// ============================================================
//  mod_elm327 - BLE ELM327 Emulation
//
//  Emuliert einen Vgate iCar Pro (BLE 4.0) - von ABRP empfohlen
//
//  BLE GATT Profile (identisch mit Vgate iCar Pro):
//   Service:   FFE0 (oder 18F0 bei neueren Versionen)
//   Notify:    FFE1  ← ESP32 sendet Antworten
//   Write:     FFE1  ← App sendet AT/OBD Befehle
//
//  Protokoll-Fluss:
//   App → "ATZ\r"          → ESP32 → "ELM327 v1.5\r\n>"
//   App → "ATE0\r"         → ESP32 → "OK\r\n>"
//   App → "ATSP6\r"        → ESP32 → "OK\r\n>"  (CAN 500kbps)
//   App → "0100\r"         → ESP32 → CAN anfragen → Antwort
//   App → "010C\r"         → ESP32 → RPM
//   App → "010D\r"         → ESP32 → Speed
//   App → "2280\r"         → ESP32 → UDS 0x22 DID 0x0280 (SoC)
// ============================================================

// BLE Device Name (so erscheint er in ABRP)
#define ELM327_BLE_NAME     "OBDII"

// GATT UUIDs (Vgate iCar Pro kompatibel)
#define ELM327_SERVICE_UUID  "FFE0"
#define ELM327_CHAR_UUID     "FFE1"   // Notify + Write Combined

void elm327_init();
bool elm327_connected();