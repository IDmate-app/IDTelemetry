#pragma once

#include <stdint.h>

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
void gps_ext_sleep();     // UART schliessen, Pins hochohmig, DC5 aus (Hardware-Backup)
void gps_ext_standby_test();  // Serial-Befehl "gps standby": Standby+Wake nachmessen
void gps_ext_powercycle_test(uint32_t off_s);  // "gps powercycle <s>": DC5 aus/an, TTFF messen
void gps_ext_dump_group(uint16_t group);       // "gps cfg <hex>": Konfigurationsgruppe ausgeben
void gps_ext_raw_proof();                      // "gps raw": rohe GGA/RMC mit Schluesselfeldern
void gps_ext_gsv_proof();                      // "gps gsv": rohe GSV-Saetze + was der Parser liest
bool gps_ext_ok();        // true = Modul gefunden + mind. 1 gültiger Fix
int  gps_ext_sat_count();     // Satelliten in use, -1 = noch kein Fix
int  gps_ext_sat_visible();   // Satelliten in view (alle Konstellationen)
