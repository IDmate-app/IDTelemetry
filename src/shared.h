#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"

// ============================================================
//  Gemeinsame Typen
// ============================================================

struct LogEntry {
    uint32_t timestamp;
    bool     is_tx;
    uint32_t can_id;
    uint8_t  len;
    uint8_t  data[8];
    char     info[LOG_INFO_LEN];
};

struct UDSModule {
    const char* name;
    uint32_t    req_id;
    uint32_t    resp_id;
};

struct KnownDID {
    uint16_t    did;
    const char* name;
    const char* unit;
};

struct ScanParams {
    int      mode;
    uint32_t req_id;
    uint32_t resp_id;
    char     name[40];
};

// ============================================================
//  VAG UDS Steuergeräte (aus ODIS extrahiert)
// ============================================================
extern const UDSModule VAG_MODULES[];
extern const int       VAG_MODULE_COUNT;

// ============================================================
//  VW MEB bekannte DIDs
// ============================================================
extern const KnownDID  VW_MEB_DIDS[];
extern const int       VW_MEB_DID_COUNT;

// ============================================================
//  GPS-Cache — zentraler Shared-State (Spinlock-geschützt)
//  Schreiber: mod_modem (gps_update via AT+CGNSINF)
//  Leser:     mod_web, mod_headers, mod_traccar, ...
//
//  double ist 8 Byte → nicht atomar auf ESP32 (Xtensa LX7).
//  Spinlock schützt gegen Tearing bei Cross-Core-Zugriff.
// ============================================================
struct GpsCache {
    bool   valid      = false;
    double lat        = 0.0;
    double lon        = 0.0;
    float  speed_kmh  = 0.0f;  // Geschwindigkeit (km/h)
    float  course_deg = 0.0f;  // Kurs (°, berechnet aus letzten zwei Fixes)
    char   loc[32]    = "";    // "51.123456 11.123456"
};
extern GpsCache g_gps;

// Atomarer Snapshot — alle Felder konsistent in einem Rutsch lesen
struct GpsSnapshot {
    bool   valid;
    double lat;
    double lon;
    float  speed_kmh;
    float  course_deg;
    char   loc[32];
};

// Schreiber (nur mod_modem): setzt alle Felder atomar
void gps_update(double lat, double lon, float speed_kmh, float course_deg);
void gps_invalidate();

// Leser: einzelne Felder (Spinlock-geschützt)
bool        gps_valid();
double      gps_lat();
double      gps_lon();
const char* gps_location_str();     // Pointer auf static Buffer (thread-safe Kopie)

// Leser: konsistenter Snapshot aller Felder auf einmal
GpsSnapshot gps_snapshot();

// ============================================================
//  Globale Objekte (definiert in main.cpp)
// ============================================================
extern AsyncWebServer server;
extern AsyncWebSocket ws;
extern SemaphoreHandle_t logMutex;

// Scan State (definiert in mod_can.cpp)
extern volatile bool scan_running;
extern volatile bool scan_abort;
extern uint16_t      scan_step;
extern uint16_t      scan_total;

// CAN State (definiert in mod_can.cpp)
extern bool can_running;
extern bool monitor_mode;

// Log (definiert in mod_logs.cpp)
extern LogEntry* log_buf;
extern uint16_t  log_count;