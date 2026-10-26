#pragma once
#include <stdint.h>
#include "mod_telemetry.h"  // TelemetryRow

// ============================================================
//  mod_telem_store — Raw-Partition Ring-Buffer fuer Telemetrie
//
//  Eigene 5 MB Flash-Partition (Subtype 0x40), kein Filesystem.
//  Jede Zeile in 128-Byte-Slot, 32 Slots pro 4 KB Erase-Block.
//  Status-Byte pro Slot ermoeglicht crash-sichere Atomik:
//    0xFF = leer, 0xFE = geschrieben, 0xFC = gesendet/ack'd
//  Vor jedem Write wird der Ziel-Slot geprueft: ist er nicht 0xFF, wird der
//  betroffene 4-KB-Block (bzw. der naechste) erased, bevor geschrieben wird
//  (FIXES B.1). reserved[0] traegt eine Layout-Version (FIXES B.5).
//
//  Drop-in Ersatz fuer die alten spiffs_q_* Funktionen.
// ============================================================

// Init: Partition finden, Boot-Scan (head/tail bestimmen).
// Muss VOR telem_init() aufgerufen werden (oder am Anfang davon).
void telem_store_init();

// Eine Zeile anhaengen. Bei Buffer-Full wird der aelteste Block
// (32 Zeilen) geloescht — Datenverlust nur wenn >40k Rows ungesendet.
bool telem_store_append(const TelemetryRow& row);

// Aelteste ungesendete Zeile lesen (ohne entfernen).
bool telem_store_peek(TelemetryRow& out);

// Aelteste Zeile als gesendet markieren.
void telem_store_ack();

// Anzahl ungesendeter Zeilen.
uint32_t telem_store_pending();

// Max-Kapazitaet (Diagnose).
uint32_t telem_store_capacity();
