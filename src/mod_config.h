#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
//  mod_config — Laufzeit-Konfiguration via NVS (ESP32 Preferences)
//
//  cfg_init() beim Start aufrufen (vor web_init()).
//  Lädt alle Werte aus NVS; bei leerem NVS greifen die
//  Compile-Zeit-Defaults aus secrets.h.
//
//  Änderungen via /api/config POST → sofort in NVS + RAM.
//  AP-SSID/Passwort + APN: erst nach Neustart aktiv.
// ============================================================

void        cfg_init();

// WiFi Access Point
const char* cfg_ap_ssid();
const char* cfg_ap_pass();

// SIM / APN
const char* cfg_sim_pin();      // SIM PIN (leer = kein PIN)
const char* cfg_apn();
const char* cfg_apn_user();
const char* cfg_apn_pass();

// Traccar OsmAnd
const char* cfg_traccar_host();
const char* cfg_traccar_id();

// InfluxDB v2
const char* cfg_influx_host();
const char* cfg_influx_org();
const char* cfg_influx_bucket();
const char* cfg_influx_token();
const char* cfg_influx_device();

// MQTT Broker
const char* cfg_mqtt_host();
uint16_t    cfg_mqtt_port();
const char* cfg_mqtt_user();
const char* cfg_mqtt_pass();
const char* cfg_mqtt_topic();    // Topic-Prefix (z.B. "tele/id7")

// AES-256 key (64 hex chars). Empty string = fall back to SECRET_AES_KEY
// from secrets.h. Setting this via the Web UI overrides the compile-time
// key without re-flashing.
const char* cfg_aes_key();

// WiFi-Upload (STA) — two priority slots (e.g. home WiFi + phone hotspot).
// mod_wifi_upload joins whichever slot's SSID is in range; slot 1 wins ties.
const char* cfg_sta_ssid();        // slot 1
const char* cfg_sta_pass();
const char* cfg_upload_url();      // HTTP(S) endpoint for slot 1
const char* cfg_sta_ssid_2();      // slot 2
const char* cfg_sta_pass_2();
const char* cfg_upload_url_2();    // HTTP(S) endpoint for slot 2

// GPS source: "ext" (default, BLITZ M10), "int" (SIM7080G GNSS), "off"
const char* cfg_gps_src();

// BLE Standby-Defaults (ABRP bekommt Werte auch ohne CAN)
bool        cfg_ble_standby();

// Sprache ("de", "en")
const char* cfg_lang();

// Modul-Schalter (0 = nicht vorhanden, 1 = vorhanden)
bool        cfg_mod_gps();      // Internes GPS (SIM7080G GNSS)
bool        cfg_mod_compass();  // FIXES B.17: QMC5883L-Kompass (Setting persistiert; Treiber folgt)

// Log-Schalter (SPIFFS-Logging ein/aus, zur Laufzeit änderbar)
bool        cfg_log_can();    // CAN/ELM SPIFFS-Log
bool        cfg_log_ble();    // BLE SPIFFS-Log
bool        cfg_log_wifi();   // WiFi Scan-Log (Legacy)

// Web-API: POST-Body (JSON) in NVS schreiben + RAM aktualisieren
bool        cfg_save_json(const uint8_t* body, size_t len);

// Web-API: aktuellen Stand als JSON-String (statischer Puffer)
const char* cfg_to_json();
