#pragma once
#include "shared.h"

// ============================================================
//  mod_logs - Zentrales Log-Modul
//
//  Alle SPIFFS-Logs an einem Ort:
//   /scan.log  — CAN-Scanner (Web, manuell, UDS)  max 256KB
//   /elm.log   — ELM327/ABRP Anfragen + Antworten max 256KB
//   /ble.log   — BLE Geräte-Snapshots             max 128KB
//   /sys.log   — Systemereignisse                 max  64KB
//
//  RAM Ring-Buffer: 200 Einträge (nur SCAN, für Live-Anzeige)
//  Zeitstempel: rtc_now_str() — echte Uhrzeit wenn RTC läuft
// ============================================================

// Init — einmalig nach SPIFFS.begin() aufrufen
void logs_init();

// ---- CAN + ELM Log ----------------------------------------

#define LOG_SRC_SCAN      0    // Web-Scanner
#define LOG_SRC_ELM       1    // ELM327 / ABRP

#define SPIFFS_SCAN_LOG   "/scan.log"
#define SPIFFS_ELM_LOG    "/elm.log"
#define SPIFFS_LOG_MAX_KB 256

// Eintrag hinzufügen (src = LOG_SRC_SCAN oder LOG_SRC_ELM)
void   log_add(bool tx, uint32_t id, uint8_t* d, uint8_t len,
               const char* info, uint8_t src = LOG_SRC_SCAN);
void   log_clear(uint8_t src = LOG_SRC_SCAN);
String log_to_txt(uint8_t src = LOG_SRC_SCAN);

// WebSocket Scan-Status
void ws_scan_status(const char* msg, uint16_t step, uint16_t total);

// ---- BLE Log ----------------------------------------------

#define SPIFFS_BLE_LOG        "/ble.log"
#define SPIFFS_BLE_LOG_MAX_KB 128

void   log_ble_snapshot(const char* json_devices);
String log_ble_to_txt();
void   log_ble_clear();

// ---- System Log -------------------------------------------

#define SPIFFS_SYS_LOG    "/sys.log"
#define SPIFFS_SYS_LOG_KB 768   // Worst-Case: sys+scan+elm+ble+telem_q+HTML ~1.7MB von 2.0MB
                                // → unter 85% Belegung damit SPIFFS-GC nicht einbricht.

void        syslog(const char* category, const char* msg);
const char* syslog_timestr();

// ---- SPIFFS-Mutex -----------------------------------------
// Schützt alle SPIFFS-Zugriffe gegen concurrent open/read/write/rename.
// Nicht-thread-safe SPIFFS war Ursache für Heap-Korruption + Scheduler-Crashes.
bool spiffs_lock(uint32_t timeout_ms = 500);
void spiffs_unlock();
