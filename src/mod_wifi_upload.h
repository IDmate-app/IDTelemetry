#pragma once
#include <stdint.h>
#include "config.h"

// ============================================================
//  mod_wifi_upload — WiFi STA upload of buffered telemetry rows
//
//  Used by both s3_full (as a secondary path when at home / on the
//  phone hotspot) and s3_lite (as the only upload path).
//
//  Supports TWO priority slots (e.g. home WiFi + phone hotspot).
//  On every reconnect attempt we passively scan the air, pick the
//  highest-priority slot whose SSID is in range, and join it. Empty
//  SSID = slot disabled.
//
//  Status: connect logic is live; HTTP-POST drain of telem_store is
//  still a TODO — task currently logs the lifecycle so the AP/STA
//  flow can be observed in the syslog.
// ============================================================

#if FEATURE_WIFI_UPLOAD

#define WIFI_UPLOAD_SLOTS 2

// Configure a single slot. Pass empty ssid to disable that slot.
// Slot 0 has priority over slot 1 when both are in range.
// All strings are copied to RAM — callers can free their buffers.
void wifi_upload_configure_slot(int slot, const char* ssid, const char* pass,
                                const char* post_url);

// Start the upload task (FreeRTOS, Core 0, low priority). Idempotent.
void wifi_upload_start_task();

// Is the STA currently associated?
bool wifi_upload_is_connected();

// Which slot are we currently on (0 / 1), or -1 if disconnected.
int  wifi_upload_active_slot();

// How many rows we have drained successfully since boot.
uint32_t wifi_upload_count();

// Web UI test trigger — kicks off a one-shot connect + dummy publish
// against the given slot. Runs asynchronously; check the syslog for
// the result. Returns false if slot index is invalid or already busy.
bool wifi_upload_test_slot(int slot);

#else

static inline void wifi_upload_configure_slot(int, const char*, const char*, const char*) {}
static inline void wifi_upload_start_task()           {}
static inline bool wifi_upload_is_connected()         { return false; }
static inline int  wifi_upload_active_slot()          { return -1; }
static inline uint32_t wifi_upload_count()            { return 0; }
static inline bool wifi_upload_test_slot(int)         { return false; }

#endif
