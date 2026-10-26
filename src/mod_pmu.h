#pragma once
#include "config.h"

// ============================================================
//  mod_pmu - AXP2101 PMU (LILYGO T-SIM7080G-S3)
//
//  I2C-Bus: Wire1  SDA=PMU_SDA_PIN (GPIO15)  SCL=PMU_SCL_PIN (GPIO7)
//  Adresse: 0x34
//
//  Lite-Variante hat keinen AXP2101 → alle Aufrufe werden zu inline no-ops.
//  Aufrufer (mod_sleep, main.cpp) brauchen so keine #ifdef-Wrapper.
// ============================================================

#if FEATURE_PMU

void pmu_init();
void pmu_update();    // frischen I2C-Abruf
int  pmu_batt_pct();  // -1 = kein Akku / PMU nicht gefunden, 0-100 = %
bool pmu_ok();        // FIXES B.10: true = AXP2101 gefunden + initialisiert
bool pmu_is_vbus_in();   // true = externe Spannung am USB/VBUS angeschlossen
bool pmu_is_charging();  // true = Akku wird gerade geladen
void pmu_set_charging(bool on);      // Laden ein/ausschalten (fuer Deep Sleep)
void pmu_set_gps_power(bool on);     // BLDO2 (GPS-Antenne) ein/ausschalten
void pmu_set_modem_power(bool on);   // DC3 (Modem VDD) ein/ausschalten
void pmu_set_ext_power(bool on);     // DC5 (ext. GPS) ein/ausschalten
void pmu_enable_vbus_wake();         // VBUS-Insert IRQ aktivieren (vor Deep Sleep)
void pmu_clear_wake_irq();           // Pending IRQ löschen (nach EXT0-Wake)
bool pmu_is_dc5_on();                // Debug: DC5-Register prüfen

#else  // FEATURE_PMU == 0 — Lite variant has no AXP2101

static inline void pmu_init()                     {}
static inline void pmu_update()                   {}
static inline int  pmu_batt_pct()                 { return -1; }
static inline bool pmu_ok()                       { return true; }  // Stub deterministisch
static inline bool pmu_is_vbus_in()               { return true; }  // assume USB-powered
static inline bool pmu_is_charging()              { return false; }
static inline void pmu_set_charging(bool)         {}
static inline void pmu_set_gps_power(bool)        {}
static inline void pmu_set_modem_power(bool)      {}
static inline void pmu_set_ext_power(bool)        {}
static inline void pmu_enable_vbus_wake()         {}
static inline void pmu_clear_wake_irq()           {}
static inline bool pmu_is_dc5_on()                { return false; }

#endif
