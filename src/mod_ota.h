#pragma once
#include <stdint.h>
#include <stddef.h>
#include <ArduinoJson.h>

// ============================================================
//  mod_ota — Firmware-Update ueber die Weboberflaeche (ab 1.6.0)
//
//  Zwei App-Plaetze (ota_0 / ota_1): der Upload schreibt in den freien,
//  der laufende bleibt als Rueckweg erhalten.
//
//  Absturzschutz: stuerzt eine frisch hochgeladene Firmware dreimal in Folge
//  ab (Panic/Watchdog), bevor sie 2 Minuten lief, startet der Stick wieder die
//  vorherige. Deep-Sleep-Wakes und normale Neustarts zaehlen nicht.
// ============================================================

void ota_boot_guard();          // ganz frueh in setup()
void ota_stable_tick();         // in loop(): nach 2 min Laufzeit = bewaehrt
void ota_boot_log();            // nach logs_init(): Rueckfall/Slot ins Syslog
bool ota_possible();            // gibt es einen zweiten App-Platz?
void ota_status_json(JsonDocument& doc);

// Upload (vom Webserver aufgerufen). err: kurzer Fehlercode oder "".
bool ota_upload_begin(const char* filename, const char* pw, const uint8_t* first, size_t len);
bool ota_upload_write(const uint8_t* data, size_t len);
bool ota_upload_end();
const char* ota_last_error();
bool ota_upload_ok();           // letzter Upload vollstaendig + geprueft

// Zurueck zur Firmware im anderen Platz. false = kein gueltiges Image dort.
bool ota_rollback(const char* pw, const char** err);
