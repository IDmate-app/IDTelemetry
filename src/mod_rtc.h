#pragma once
#include "shared.h"

// ============================================================
//  mod_rtc - DS1307 RTC Modul
//
//  Verdrahtung:
//   VCC → ESP32 3.3V
//   GND → ESP32 GND
//   SDA → GPIO45 (RTC_SDA_PIN in config.h)
//   SCL → GPIO21 (RTC_SCL_PIN in config.h)
//   BAT → CR2032 einlegen
//
//  I2C Adressen: DS1307=0x68, AT24C32=0x50 (ungenutzt)
//  Pullups 4.7kΩ bereits auf Modul vorhanden.
// ============================================================

void   rtc_init();
bool   rtc_set_time(int h, int m, int s);              // nur Uhrzeit (Kompatibilität)
bool   rtc_set_datetime(int y, int mo, int d, int h, int mi, int s);  // Datum + Uhrzeit
bool   rtc_get_time(int& h, int& m, int& s);
bool   rtc_get_datetime(int& y, int& mo, int& d, int& h, int& mi, int& s);
bool   rtc_is_running();
String rtc_time_str();   // "HH:MM:SS" oder "--:--:--"

// Unix-Timestamp (Sekunden seit 1970-01-01) — 0 wenn kein Datum gesetzt
uint32_t rtc_unix_timestamp();
// Unix-Timestamp in Millisekunden für InfluxDB — 0 wenn kein Datum gesetzt
uint64_t rtc_unix_ms();

// Zentrale Zeitreferenz — wird alle 60s von RTC kalibriert
void     rtc_time_sync_task_init();   // startet den 60s Sync-Task
String   rtc_now_str();               // "HH:MM:SS.mmm" oder "+HH:MM:SS.mmm"
uint32_t rtc_now_ms_of_day();         // ms seit Mitternacht, für Berechnungen