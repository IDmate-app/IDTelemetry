#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================
//  mod_didsearch - DID-Suche, Sammeln und Messlaeufe (1.7.9 / 1.7.10 / 1.7.11)
// ============================================================
//  Alles nur lesend (UDS 0x22) und nur, solange der Stick senden darf.
//
//  Suche:   ein Bereich auf einem Steuergeraet, Antworten ins Syslog, Treffer
//           gegen die eingetragene Reichweite markiert (1.7.9).
//  Sammeln: feste Liste von Steuergeraeten und Bereichen; jede Kennung, die
//           antwortet, kommt nach /did_known.txt ("REQ RESP DID" je Zeile).
//           Ein Bereich, der 32x am Stueck schweigt, wird uebersprungen.
//  Messen:  (1.7.11: sammelt zuerst, wenn noch keine Liste da ist) liest alle gesammelten Kennungen einmal und haengt eine Zeile an
//           /did_runs.jsonl: Zeit, angezeigte km, SoC, km-Stand, Temperaturen
//           und alle Rohantworten. Mehrere Messungen bei verschiedener
//           Reichweite werten tools/did_fit.py aus.

bool didsearch_start(uint32_t req_id, uint32_t resp_id, uint16_t from, uint16_t to, int target);
bool didsearch_collect();
bool didsearch_measure(int target_km);
void didsearch_abort();
void didsearch_status(JsonDocument& doc);
void didsearch_clear_runs();
void didsearch_clear_known();

// Weck-Test (1.7.10): eine Lese-Anfrage auch ohne Spannung am Port
// (Hoermodus kurz aus), Ergebnis ins Syslog. Laeuft im Hintergrund.
bool didsearch_wake_test();

#define DID_KNOWN_FILE "/did_known.txt"
#define DID_RUNS_FILE  "/did_runs.jsonl"
