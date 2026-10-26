#pragma once

// ============================================================
//  mod_gps_ext — externes u-blox GPS
//
//  UART2: RX=GPIO1 (< GPS TX), TX=GPIO2 (> GPS RX)
//  Versorgung ueber PMU-Rail DC5 (3,3 V). Kein I2C — der QWIIC-Bus
//  (SDA=GPIO45, SCL=GPIO21) wird von diesem Modul nicht benutzt.
//
//  Nur aktiv wenn GPS_EXT_ENABLED == true (config.h).
// ============================================================

void gps_ext_wake();      // UART öffnen + Wake-Byte — so früh wie möglich im Boot aufrufen!
void gps_ext_init();      // Config + Task starten — kann später kommen
void gps_ext_sleep();     // UART schliessen (Backup-Mode + DC5 aus folgt separat)
bool gps_ext_ok();        // true = Modul gefunden + mind. 1 gültiger Fix
int  gps_ext_sat_count();     // Satelliten in use, -1 = noch kein Fix
int  gps_ext_sat_visible();   // Satelliten in view (alle Konstellationen)
