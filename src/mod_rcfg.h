#pragma once
#include <stddef.h>
#include <ArduinoJson.h>

// ============================================================
//  mod_rcfg — von IDmate gelernte Korrekturen (1.6.8)
//
//  IDmate lernt mit "BMS lernen", was das Auto im Display zeigt, und schickt
//  das Ergebnis retained auf <topic>/cfg: base64 eines Rahmens wie bei den
//  Telegrammen ([0x20][IV][AES-256-CBC], derselbe Schluessel) um JSON
//    {"o": km-Offset, "c": [c0, c1, ...], "t": "c[b][e]"}
//  Anzeige-SoC = c0 + c1*CAN + c2*(Akku-Temp - 20) + c3*(Aussen-Temp - 10),
//  Terme in der Reihenfolge von "t". Leeres Objekt = Korrekturen aus.
//
//  Angewandt NUR auf die Antworten an ABRP (BLE/ELM). Was der Stick an
//  IDmate sendet, bleibt roh — das Lernen braucht die CAN-Werte.
// ============================================================

void  rcfg_init();                              // gespeicherten Stand aus NVS
bool  rcfg_apply_b64(const char* b64, size_t len);   // Nachricht von <topic>/cfg
float rcfg_abrp_soc(float can_soc);             // Anzeige-SoC fuer ABRP (oder unveraendert)
float rcfg_abrp_odo(float can_km);              // Tacho-km fuer ABRP (oder unveraendert)
void  rcfg_status_json(JsonDocument& doc);      // fuer /status
