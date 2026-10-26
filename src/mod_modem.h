#pragma once
#include <stdint.h>
#include "config.h"

// ============================================================
//  mod_modem - SIM7080G Modem (LTE + GPS)
//
//  Verwaltet UART1 (MODEM_TX/RX aus config.h) exklusiv.
//  GPS-Daten → g_gps (shared.h), Inline-API: gps_valid/lat/lon/location_str
//
//  Lite-Variante hat kein SIM7080G → alle Aufrufe werden zu inline no-ops.
// ============================================================

#if FEATURE_MODEM

void modem_init();
void modem_start_task();
bool modem_ensure_connected();
bool modem_is_connected();

int8_t      modem_signal_quality();
const char* modem_operator();
uint16_t    modem_plmn();
bool        modem_sim_ok();

void modem_poweroff();
void modem_pre_sleep_flush();

int  modem_gps_vsat();
int  modem_gps_usat();

void modem_print_gps_info();
void modem_print_lte_info();
void modem_print_lte_sig();
void modem_print_lte_scan();
void modem_print_lte_bands();
void modem_lte_bands_fix(bool all);
void modem_send_at(const char* cmd);
void modem_pause_task();
void modem_resume_task();
void modem_clear_reboot_count();   // FIXES B.17: WD-Reboot-Counter (RTC_DATA) beim Sleep nullen

bool modem_mqtt_connected();

#else  // FEATURE_MODEM == 0 — Lite variant has no SIM7080G

static inline void modem_init()              {}
static inline void modem_start_task()        {}
static inline bool modem_ensure_connected()  { return false; }
static inline bool modem_is_connected()      { return false; }
static inline int8_t      modem_signal_quality() { return -1; }
static inline const char* modem_operator()       { return ""; }
static inline uint16_t    modem_plmn()           { return 0; }
static inline bool        modem_sim_ok()         { return false; }
static inline void modem_poweroff()         {}
static inline void modem_pre_sleep_flush()  {}
static inline int  modem_gps_vsat()         { return 0; }
static inline int  modem_gps_usat()         { return 0; }
static inline void modem_print_gps_info()   {}
static inline void modem_print_lte_info()   {}
static inline void modem_print_lte_sig()    {}
static inline void modem_print_lte_scan()   {}
static inline void modem_print_lte_bands()  {}
static inline void modem_lte_bands_fix(bool) {}
static inline void modem_send_at(const char*) {}
static inline void modem_pause_task()       {}
static inline void modem_resume_task()      {}
static inline void modem_clear_reboot_count() {}
static inline bool modem_mqtt_connected()   { return false; }

#endif
