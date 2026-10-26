#pragma once

#include "config.h"
#include "mod_telemetry.h"

// ============================================================
//  mod_mqtt — MQTT via SIM7080G nativen AT-Client
//  Lite-Variante hat kein MQTT → no-op stubs.
// ============================================================

#if FEATURE_MODEM

void mqtt_configure();
bool mqtt_connect();
bool mqtt_is_connected();
bool mqtt_publish_row(const TelemetryRow& row);
void mqtt_poll();
void mqtt_disconnect();
bool        mqtt_ok();
uint32_t    mqtt_last_pub_ms();
uint16_t    mqtt_pub_count();
const char* mqtt_last_ack();
void mqtt_print_info();

#else

#include <stdint.h>
static inline void mqtt_configure()                       {}
static inline bool mqtt_connect()                         { return false; }
static inline bool mqtt_is_connected()                    { return false; }
static inline bool mqtt_publish_row(const TelemetryRow&)  { return false; }
static inline void mqtt_poll()                            {}
static inline void mqtt_disconnect()                      {}
static inline bool        mqtt_ok()           { return false; }
static inline uint32_t    mqtt_last_pub_ms()  { return 0; }
static inline uint16_t    mqtt_pub_count()    { return 0; }
static inline const char* mqtt_last_ack()     { return ""; }
static inline void mqtt_print_info()                      {}

#endif
