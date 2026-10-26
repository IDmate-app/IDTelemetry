#pragma once
#include <stdint.h>
#include "mod_telemetry.h"

// Field bitmask — kept in sync with the server decoder.
//
// CRITICAL: the server reads set bits in ascending order and consumes
// the matching number of bytes for each. PARKED, CHARGING and DCFC are
// mask-only (no payload bytes). Reordering these bit positions corrupts
// every field that comes after — that's exactly the bug that produced
// ODO=4,489,490 km / RANGE=2458 km / PLMN=22870 in v1.2.0.
//
// DO NOT TOUCH the bit numbers without coordinating with the server-side
// decoder in IDMate. PARKED stays at bit 15.
enum BinField : uint32_t {
    BF_LAT       = (1u << 0),
    BF_LON       = (1u << 1),
    BF_HEADING   = (1u << 2),
    BF_SOC       = (1u << 3),
    BF_VOLTAGE   = (1u << 4),
    BF_CURRENT   = (1u << 5),
    BF_POWER     = (1u << 6),
    BF_SPEED     = (1u << 7),
    BF_CHARGING  = (1u << 8),   // mask-only
    BF_DCFC      = (1u << 9),   // mask-only
    BF_BATT_TEMP = (1u << 10),
    BF_EXT_TEMP  = (1u << 11),
    BF_RANGE     = (1u << 12),
    BF_CAPACITY  = (1u << 13),
    BF_KWH       = (1u << 14),
    BF_PARKED    = (1u << 15),  // mask-only
    BF_ODOMETER  = (1u << 16),
    BF_LTE_SIG   = (1u << 17),
    BF_BATT_DEV  = (1u << 18),
    BF_LTE_PLMN  = (1u << 19),
};

// ============================================================
//  mod_payload — shared AES-256-CBC encrypted binary payload
//
//  Used by BOTH upload paths so the server sees identical bytes
//  regardless of transport:
//    - mod_mqtt        → MQTT over LTE (full variant)
//    - mod_wifi_upload → HTTP POST or MQTT over WiFi (both variants)
//
//  Wire format produced by payload_encode():
//    [0x02][IV 16B][AES-CBC ciphertext with PKCS7 padding]
//    (v2: TELEM_KWH_CHARGED ist u32/10; IDMate dekodiert v1+v2)
//
//  Plaintext layout inside the ciphertext:
//    [mask u32 LE][unix_s u32 LE][packed fields per BF_* bitmask]
// ============================================================

// Build the binary plaintext + encrypt it with AES-256-CBC.
// `out` must hold at least 96 bytes (max plaintext + IV + version).
// Returns the total payload length, or 0 on error.
int  payload_encode(const TelemetryRow& row, uint8_t* out, int out_size);

// Initialise the AES key from cfg_aes_key() with secrets.h fallback.
// Idempotent — payload_encode() calls this automatically the first time.
void payload_init_key();
