#include "mod_payload.h"
#include "config.h"
#include <math.h>
#include "mod_logs.h"
#include <stdio.h>
#include "mod_telemetry.h"
#include "mod_config.h"
#include "mod_rcfg.h"
#include "mod_pmu.h"
#include "secrets.h"
#include <aes/esp_aes.h>
#include <esp_random.h>
#include <string.h>
#include <math.h>

// BF_* enum lives in mod_payload.h (shared with mod_mqtt for debug logging).

static uint8_t s_aes_key[32];
static volatile bool s_aes_ready   = false;
static bool          s_key_example = false;   // 1.6.3: Schluessel = alles 0

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void payload_init_key() {
    const char* override_key = cfg_aes_key();
    const char* hex = (override_key && strlen(override_key) >= 64) ? override_key : SECRET_AES_KEY;
    for (int i = 0; i < 32; i++) {
        s_aes_key[i] = (hex_nibble(hex[i*2]) << 4) | hex_nibble(hex[i*2+1]);
    }
    bool zero = true;
    for (int i = 0; i < 32; i++) if (s_aes_key[i]) { zero = false; break; }
    s_key_example = zero;
    s_aes_ready = true;
}

// 1.6.3: Der neue Schluessel gilt beim naechsten Telegramm, ohne Neustart.
void payload_key_changed() { s_aes_ready = false; }

bool payload_key_is_example() {
    if (!s_aes_ready) payload_init_key();
    return s_key_example;
}

// Release-Builds bringen den oeffentlichen Beispielschluessel (alles 0) mit.
// Damit verschluesselte Telegramme kann der Server nicht lesen, sie gingen
// still verloren (2026-11-06). Darum: nicht senden, Zeilen bleiben im Puffer,
// und alle 5 Minuten eine Warnung.
static void warn_example_key() {
    static uint32_t s_ms = 0;
    uint32_t now = millis();
    if (s_ms != 0 && now - s_ms < 300000UL) return;
    s_ms = now ? now : 1;
    syslog("PAYLOAD", "!!!! KEIN AES-Key: Beispielschluessel (alles 0) aktiv - es wird NICHT gesendet. "
                      "Schluessel in der Konfiguration eintragen, die Zeilen bleiben im Puffer.");
}

static inline void put_u8 (uint8_t* p, uint8_t  v) { p[0]=v; }
static inline void put_i8 (uint8_t* p, int8_t   v) { p[0]=(uint8_t)v; }
static inline void put_u16(uint8_t* p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static inline void put_i16(uint8_t* p, int16_t  v) { put_u16(p,(uint16_t)v); }
static inline void put_u32(uint8_t* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static inline void put_i32(uint8_t* p, int32_t  v) { put_u32(p,(uint32_t)v); }

// Build the plaintext payload (mask + ts + fields) into buf.
static int build_plaintext(const TelemetryRow& row, uint8_t* buf, int buf_size) {
    if (buf_size < 8) return 0;
    uint32_t mask = 0;
    int pos = 8;

    if (row.valid[TELEM_GPS_LAT] && pos + 4 <= buf_size) {
        mask |= BF_LAT; put_i32(buf + pos, (int32_t)(row.values[TELEM_GPS_LAT] * 1e6f)); pos += 4;
    }
    if (row.valid[TELEM_GPS_LON] && pos + 4 <= buf_size) {
        mask |= BF_LON; put_i32(buf + pos, (int32_t)(row.values[TELEM_GPS_LON] * 1e6f)); pos += 4;
    }
    if (row.valid[TELEM_GPS_HEADING] && pos + 2 <= buf_size) {
        // FIXES B.17: defensiver Clamp auf 0..359 (Wire ist u16 Grad) — ein
        // Heading außerhalb [0,360) würde sonst beim u16-Cast wrappen.
        float hdg = fmodf(row.values[TELEM_GPS_HEADING], 360.0f);
        if (hdg < 0.0f) hdg += 360.0f;
        mask |= BF_HEADING; put_u16(buf + pos, (uint16_t)(int)hdg); pos += 2;
    }
    if (row.valid[TELEM_SOC] && pos + 2 <= buf_size) {
        // SoC wire-format ist u16/10 → negative BMS-Werte (-5..0) würden zu Wraparound (~6550 %) führen.
        float soc_w = row.values[TELEM_SOC];
        if (soc_w < 0.f)   soc_w = 0.f;
        if (soc_w > 100.f) soc_w = 100.f;
        mask |= BF_SOC; put_u16(buf + pos, (uint16_t)(soc_w * 10.0f + 0.5f)); pos += 2;
    }
    if (row.valid[TELEM_VOLTAGE] && pos + 2 <= buf_size) {
        mask |= BF_VOLTAGE; put_u16(buf + pos, (uint16_t)row.values[TELEM_VOLTAGE]); pos += 2;
    }
    if (row.valid[TELEM_CURRENT] && pos + 2 <= buf_size) {
        mask |= BF_CURRENT; put_i16(buf + pos, (int16_t)row.values[TELEM_CURRENT]); pos += 2;
    }
    if (row.valid[TELEM_POWER] && pos + 2 <= buf_size) {
        mask |= BF_POWER; put_i16(buf + pos, (int16_t)(row.values[TELEM_POWER] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_VEHICLE_SPEED] && pos + 2 <= buf_size) {
        mask |= BF_SPEED; put_u16(buf + pos, (uint16_t)(row.values[TELEM_VEHICLE_SPEED] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_IS_CHARGING] && row.values[TELEM_IS_CHARGING] > 0.5f) mask |= BF_CHARGING;
    if (row.valid[TELEM_IS_DCFC]     && row.values[TELEM_IS_DCFC]     > 0.5f) mask |= BF_DCFC;
    if (row.valid[TELEM_IS_PARKED]   && row.values[TELEM_IS_PARKED]   > 0.5f) mask |= BF_PARKED;
    if (row.valid[TELEM_BATT_TEMP] && pos + 1 <= buf_size) {
        mask |= BF_BATT_TEMP; put_i8(buf + pos, (int8_t)row.values[TELEM_BATT_TEMP]); pos += 1;
    }
    if (row.valid[TELEM_EXT_TEMP] && pos + 1 <= buf_size) {
        mask |= BF_EXT_TEMP; put_i8(buf + pos, (int8_t)row.values[TELEM_EXT_TEMP]); pos += 1;
    }
    if (row.valid[TELEM_RANGE] && pos + 2 <= buf_size) {
        mask |= BF_RANGE; put_u16(buf + pos, (uint16_t)(row.values[TELEM_RANGE] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_CAPACITY] && pos + 2 <= buf_size) {
        mask |= BF_CAPACITY; put_u16(buf + pos, (uint16_t)(row.values[TELEM_CAPACITY] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_KWH_CHARGED] && pos + 4 <= buf_size) {
        // Wire-format v2: u32/10 — lifetime kWh counter hits 6553.5 cap in <1 year.
        float kw_w = row.values[TELEM_KWH_CHARGED];
        if (kw_w < 0.f) kw_w = 0.f;
        mask |= BF_KWH; put_u32(buf + pos, (uint32_t)(kw_w * 10.0f + 0.5f)); pos += 4;
    }
    if (row.valid[TELEM_ODOMETER] && pos + 4 <= buf_size) {
        mask |= BF_ODOMETER; put_u32(buf + pos, (uint32_t)(row.values[TELEM_ODOMETER] * 10.0f)); pos += 4;
    }
    if (row.valid[TELEM_LTE_SIGNAL] && pos + 1 <= buf_size) {
        mask |= BF_LTE_SIG; put_u8(buf + pos, (uint8_t)row.values[TELEM_LTE_SIGNAL]); pos += 1;
    }
    // FIXES B.7: Row-Wert bevorzugen, Live-Wert nur als Fallback. Sonst bekäme
    // beim Backlog-Drain (Funkloch/Store, bis 40k Rows) jede historische Row den
    // Akku-% vom SENDE-Zeitpunkt statt vom Aufnahmezeitpunkt → falsche Zeitreihe.
    int batt = row.valid[TELEM_BATT_DEVICE] ? (int)row.values[TELEM_BATT_DEVICE]
                                            : pmu_batt_pct();
    if (batt >= 0 && pos + 1 <= buf_size) {
        mask |= BF_BATT_DEV; put_u8(buf + pos, (uint8_t)batt); pos += 1;
    }
    if (row.valid[TELEM_LTE_OPERATOR] && pos + 2 <= buf_size) {
        uint32_t plmn = (uint32_t)row.values[TELEM_LTE_OPERATOR];
        if (plmn > 0 && plmn < 65536) {
            mask |= BF_LTE_PLMN; put_u16(buf + pos, (uint16_t)plmn); pos += 2;
        }
    }
    // ── 1.5.0: Bits 20-22 (Reihenfolge = Bitreihenfolge, hinten anhaengen) ──
    if (row.valid[TELEM_SOE] && pos + 2 <= buf_size) {
        float v = row.values[TELEM_SOE];
        if (v >= 0.f && v < 6500.f) {
            mask |= BF_SOE; put_u16(buf + pos, (uint16_t)(v * 10.0f + 0.5f)); pos += 2;
        }
    }
    if (row.valid[TELEM_HVAC_KW] && pos + 2 <= buf_size) {
        float v = row.values[TELEM_HVAC_KW];
        if (v > -320.f && v < 320.f) {
            mask |= BF_HVAC_KW; put_i16(buf + pos, (int16_t)lroundf(v * 100.0f)); pos += 2;
        }
    }
    if (row.valid[TELEM_CABIN_TEMP] && pos + 1 <= buf_size) {
        float v = row.values[TELEM_CABIN_TEMP];
        if (v > -100.f && v < 120.f) {
            mask |= BF_CABIN_TEMP; put_i8(buf + pos, (int8_t)lroundf(v)); pos += 1;
        }
    }
    put_u32(buf + 0, mask);
    put_u32(buf + 4, row.unix_s);
    return pos;
}

// [version][IV][AES-256-CBC(plain, PKCS7)] — gemeinsam fuer Werte und Info.
static int encrypt_frame(uint8_t version, const uint8_t* plain, int plain_len,
                         uint8_t* out, int out_size) {
    if (!s_aes_ready) payload_init_key();
    if (s_key_example) { warn_example_key(); return 0; }
    if (plain_len <= 0) return 0;

    int pad = 16 - (plain_len % 16);
    int padded_len = plain_len + pad;
    int total = 1 + 16 + padded_len;
    if (total > out_size) return 0;

    uint8_t padded[160];
    if (padded_len > (int)sizeof(padded)) return 0;
    memcpy(padded, plain, plain_len);
    memset(padded + plain_len, pad, pad);

    out[0] = version;
    uint8_t iv[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(iv + i * 4, &r, 4);
    }
    memcpy(out + 1, iv, 16);

    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, s_aes_key, 256);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int ret = esp_aes_crypt_cbc(&ctx, ESP_AES_ENCRYPT, padded_len,
                                 iv_copy, padded, out + 17);
    esp_aes_free(&ctx);
    return (ret == 0) ? total : 0;
}

// 1.6.8: Gegenstueck fuer Nachrichten von IDmate (Konfiguration). Gleicher
// Schluessel, gleicher Rahmen. -1 = falsche Version, Laenge, Schluessel oder
// Auffuellung. Mit dem Beispielschluessel wird nichts angenommen.
int payload_decrypt(uint8_t version, const uint8_t* in, int in_len, uint8_t* out, int out_size) {
    if (!s_aes_ready) payload_init_key();
    if (s_key_example) return -1;
    if (!in || in_len < 33 || in[0] != version || ((in_len - 17) % 16) != 0) return -1;
    int clen = in_len - 17;
    if (clen > out_size) return -1;
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, s_aes_key, 256);
    uint8_t iv[16];
    memcpy(iv, in + 1, 16);
    int ret = esp_aes_crypt_cbc(&ctx, ESP_AES_DECRYPT, clen, iv, in + 17, out);
    esp_aes_free(&ctx);
    if (ret != 0) return -1;
    uint8_t pad = out[clen - 1];
    if (pad < 1 || pad > 16) return -1;
    for (int i = 0; i < pad; i++) if (out[clen - 1 - i] != pad) return -1;
    return clen - pad;
}

int payload_encode(const TelemetryRow& row, uint8_t* out, int out_size) {
    uint8_t plain[96];
    int plain_len = build_plaintext(row, plain, sizeof(plain));
    if (plain_len == 0) return 0;
    // protocol v2: kw widened from u16 to u32; Bits 20-22 angehaengt (bleibt v2)
    return encrypt_frame(PAYLOAD_VERSION_VALUES, plain, plain_len, out, out_size);
}

int payload_encode_info(uint8_t* out, int out_size) {
    char json[128];
    int n = snprintf(json, sizeof(json),
                     "{\"fw\":\"%s\",\"build\":\"%s\",\"var\":\"%s\",\"pv\":%d,\"bits\":%d,\"odo\":%.0f}",
                     FW_VERSION, FW_BUILD, FW_VARIANT, PAYLOAD_VERSION_VALUES, PAYLOAD_BITS,
                     rcfg_odo_offset());   // 1.6.10: km-Versatz, den der Stick schon aufschlaegt
    if (n <= 0 || n >= (int)sizeof(json)) return 0;
    return encrypt_frame(PAYLOAD_VERSION_INFO, (const uint8_t*)json, n, out, out_size);
}
