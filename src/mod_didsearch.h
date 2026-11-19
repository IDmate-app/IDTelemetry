#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================
//  mod_didsearch - DID-Suche (1.7.9), z. B. fuer die Reichweite
// ============================================================
//  Fragt auf einem Steuergeraet alle DIDs eines Bereichs per UDS 0x22
//  (nur lesen) ab und schreibt jede positive Antwort ins Syslog (CAN).
//  Mit "target" (z. B. die im Auto angezeigte Reichweite in km) werden
//  Antworten markiert, in denen der Wert vorkommt (1 oder 2 Byte, auch x10,
//  /2, /4; +-3). Laeuft nur, solange der Stick senden darf (Auto wach).

bool didsearch_start(uint32_t req_id, uint32_t resp_id, uint16_t from, uint16_t to, int target);
void didsearch_abort();
void didsearch_status(JsonDocument& doc);
