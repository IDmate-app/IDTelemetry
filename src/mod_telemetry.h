#pragma once
#include <stdint.h>

// ============================================================
//  mod_telemetry — Telemetrie-Cache + Ring-Buffer
//
//  Phase 1: Nur Nicht-CAN-Quellen (GPS, PMU, Gyro, Modem-Status)
//  Phase 2: CAN/UDS-Abfragen (batt_temp, soc, voltage, …)
//  Phase 3: InfluxDB v2 Sender + SPIFFS-Persistenz
//
//  Öffentliche API:
//    telem_init()               — einmalig vor Task-Start
//    telem_start_task()         — FreeRTOS-Task starten
//    telem_get_latest()         — letzten Wert + Alter lesen
//    telem_get_operator_str()   — Netzanbieter-Name (nur TELEM_LTE_OPERATOR)
//    telem_get_buf_pending()    — unggesendete Punkte (alle Entitäten zusammen)
//    telem_influx_ok()          — letzter InfluxDB-Versand erfolgreich (Phase 3)
// ============================================================

// ── Alle Telemetrie-Felder ───────────────────────────────────
enum TelemField {
    // Schnell (CAN, Phase 2 — 5 s)
    TELEM_SOC = 0,
    TELEM_VOLTAGE,
    TELEM_CURRENT,
    TELEM_POWER,           // berechnet: voltage * current / 1000 (kW)
    TELEM_VEHICLE_SPEED,
    TELEM_IS_CHARGING,
    TELEM_IS_DCFC,
    // Mittel (GPS/Gyro/LTE 10 s, CAN Phase 2)
    TELEM_BATT_TEMP,
    TELEM_EXT_TEMP,
    TELEM_RANGE,           // DID 0x0295, ECU FC007B (km)
    TELEM_GPS_LAT,
    TELEM_GPS_LON,
    TELEM_GPS_HEADING,    // berechnet aus zwei aufeinanderfolgenden GPS-Punkten (°)
    TELEM_GPS_VALID,
    TELEM_GYRO_G,
    TELEM_LTE_SIGNAL,
    TELEM_LTE_OPERATOR,   // float v=0, extra str via telem_get_operator_str()
    // Langsam (CAN, Phase 2 — 30 s)
    TELEM_CAPACITY,
    TELEM_KWH_CHARGED,
    TELEM_IS_PARKED,
    // Sehr langsam (60 s)
    TELEM_ODOMETER,
    TELEM_BATT_DEVICE,
    TELEM_FIELD_COUNT
};

// ── Datenpunkt ───────────────────────────────────────────────
struct TelemetryPoint {
    uint32_t timestamp_ms;   // millis() des Messzeitpunkts
    float    value;
    bool     valid;
};


// ── Vollständige Telemetrie-Zeile (GPS-getriggert, kombiniert alle Felder) ──
struct TelemetryRow {
    uint32_t unix_s;                     // RTC-Zeitstempel bei Capture
    float    values[TELEM_FIELD_COUNT];  // Snapshot aus s_cache
    bool     valid[TELEM_FIELD_COUNT];   // Gültig + innerhalb max_age → senden
};

// ── Öffentliche API ──────────────────────────────────────────
void telem_init();
void telem_start_task();
void telem_stop_task();   // Task stoppen vor Deep Sleep

// Letzten Wert + Alter lesen (out_age_ms = millis() - timestamp)
// Gibt false zurück wenn noch kein gültiger Wert vorliegt
bool telem_get_latest(TelemField field, float* out_value, uint32_t* out_age_ms);

// Netzanbieter-Name (nur relevant für TELEM_LTE_OPERATOR)
const char* telem_get_operator_str();

// Anzahl gepufferter (noch nicht gesendeter) Punkte — alle Entitäten (Legacy, per-Feld)
uint16_t telem_get_buf_pending();

// Anzahl ausstehender Zeilen im GPS-getriggerten Zeilen-Ringpuffer
uint16_t telem_get_row_pending();

// InfluxDB-Status (Phase 3)
bool telem_influx_ok();

// Phase 3: InfluxDB-Versand (im LTE-Fenster aus mod_modem aufrufen)
void telem_force_capture(const char* reason);
void telem_send_influx();

// MQTT: älteste ungesendete Zeile lesen OHNE zu entfernen.
bool telem_peek_row(TelemetryRow& out);

// MQTT: älteste Zeile als gesendet bestätigen (nach erfolgreichem Publish/PUBACK).
void telem_ack_row();

// Phase 3: SPIFFS-Persistenz (vor Deep Sleep / nach Wake-Up)
void telem_persist_to_spiffs();
void telem_restore_from_spiffs();

// Debug: letzte N gepufferte Zeilen als lesbaren Text (für Debug-Tab)
// Gibt statischen Puffer zurück, max ~2 KB
const char* telem_preview_rows(int max_rows = 8);

// ELM327-Brücke: UDS-Antwort-Bytes (nach 0x62+DID) in Telemetrie-Cache schreiben
// Wird von mod_elm327 aufgerufen wenn ABRP eine erfolgreiche CAN-Antwort bekommt
void telem_feed_did(uint16_t did, const uint8_t* data, int data_len);
