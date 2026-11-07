#include "mod_rcfg.h"
#include "mod_payload.h"
#include "mod_logs.h"
#include "mod_telemetry.h"
#include "mod_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <math.h>
#include <string.h>

#define RCFG_VERSION 0x20

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool  s_has_odo = false;
static bool  s_has_soc = false;
static float s_odo     = 0.0f;
static float s_c[4]    = {0};
static char  s_t[4]    = "";
static int   s_nt      = 0;

static bool fin(float v) { return !isnan(v) && !isinf(v); }

// JSON pruefen und uebernehmen. false = verworfen (alter Stand bleibt).
static bool parse(const char* json, size_t len, bool log_it) {
    JsonDocument d;
    if (deserializeJson(d, json, len) || !d.is<JsonObject>()) {
        if (log_it) syslog("RCFG", "Konfiguration von IDmate unlesbar - verworfen");
        return false;
    }
    bool  has_odo = false, has_soc = false;
    float odo = 0, c[4] = {0};
    char  t[4] = "";
    int   nt = 0;

    if (!d["o"].isNull()) {
        odo = d["o"].as<float>();
        if (!fin(odo) || fabsf(odo) > 5000.f) { if (log_it) syslog("RCFG", "km-Offset unplausibel - verworfen"); return false; }
        has_odo = true;
    }
    if (!d["c"].isNull() || !d["t"].isNull()) {
        const char* ts = d["t"] | "";
        nt = strlen(ts);
        JsonArray ca = d["c"].as<JsonArray>();
        bool ok = nt >= 1 && nt <= 3 && ts[0] == 'c' && !ca.isNull() && (int)ca.size() == nt + 1;
        for (int i = 0; ok && i < nt; i++) {
            ok = (ts[i] == 'c' && i == 0) || ((ts[i] == 'b' || ts[i] == 'e') && i > 0);
            for (int k = 0; ok && k < i; k++) ok = ts[k] != ts[i];
        }
        for (int i = 0; ok && i <= nt; i++) { c[i] = ca[i].as<float>(); ok = fin(c[i]); }
        ok = ok && fabsf(c[0]) <= 30.f && c[1] >= 0.5f && c[1] <= 1.5f;
        for (int i = 2; ok && i <= nt; i++) ok = fabsf(c[i]) <= 2.f;
        if (!ok) { if (log_it) syslog("RCFG", "Anzeige-SoC-Funktion unplausibel - verworfen"); return false; }
        memcpy(t, ts, nt); t[nt] = 0;
        has_soc = true;
    }

    portENTER_CRITICAL(&s_mux);
    s_has_odo = has_odo; s_odo = odo;
    s_has_soc = has_soc; memcpy(s_c, c, sizeof(s_c)); memcpy(s_t, t, sizeof(s_t)); s_nt = nt;
    portEXIT_CRITICAL(&s_mux);

    if (log_it) {
        char m[128];
        int p = snprintf(m, sizeof(m), "Von IDmate:");
        p += has_odo ? snprintf(m + p, sizeof(m) - p, " km-Offset %+.0f", odo) : snprintf(m + p, sizeof(m) - p, " kein km-Offset");
        p += has_soc ? snprintf(m + p, sizeof(m) - p, " · Anzeige-SoC %.2f + %.4f*CAN (%s)", c[0], c[1], t)
                     : snprintf(m + p, sizeof(m) - p, " · keine Anzeige-SoC-Funktion");
        syslog("RCFG", m);
    }
    return true;
}

void rcfg_init() {
    Preferences p;
    if (!p.begin("rcfg", true)) return;
    String j = p.getString("j", "");
    p.end();
    if (j.length()) parse(j.c_str(), j.length(), false);
}

bool rcfg_apply_b64(const char* b64, size_t len) {
    static uint8_t raw[256];
    static uint8_t plain[256];
    size_t n = 0;
    if (!b64 || !len || mbedtls_base64_decode(raw, sizeof(raw), &n, (const unsigned char*)b64, len) != 0) {
        syslog("RCFG", "Konfiguration von IDmate: kein gueltiges base64");
        return false;
    }
    int m = payload_decrypt(RCFG_VERSION, raw, (int)n, plain, sizeof(plain) - 1);
    if (m < 0) {
        syslog("RCFG", "Konfiguration von IDmate nicht entschluesselbar (anderer AES-Key?)");
        return false;
    }
    plain[m] = 0;
    Preferences p;
    String old;
    if (p.begin("rcfg", true)) { old = p.getString("j", ""); p.end(); }
    if (old == (const char*)plain) return true;          // unveraendert (retained bei jedem Connect)
    if (!parse((const char*)plain, m, true)) return false;
    if (p.begin("rcfg", false)) { p.putString("j", (const char*)plain); p.end(); }
    return true;
}

float rcfg_abrp_soc(float can) {
    if (!cfg_abrp_corr()) return can;
    float c[4]; char t[4]; int nt; bool has;
    portENTER_CRITICAL(&s_mux);
    has = s_has_soc; memcpy(c, s_c, sizeof(c)); memcpy(t, s_t, sizeof(t)); nt = s_nt;
    portEXIT_CRITICAL(&s_mux);
    if (!has) return can;
    float v = c[0];
    for (int i = 0; i < nt; i++) {
        float x = 0.f;
        if (t[i] == 'c') {
            x = can;
        } else {
            float tv; uint32_t age;
            TelemField f = (t[i] == 'b') ? TELEM_BATT_TEMP : TELEM_EXT_TEMP;
            float ref    = (t[i] == 'b') ? 20.f : 10.f;   // wie bms_learn.py (BAT_REF_C, EXT_REF_C)
            x = telem_get_latest(f, &tv, &age) ? tv - ref : 0.f;
        }
        v += c[i + 1] * x;
    }
    if (v < 0.f)   v = 0.f;
    if (v > 100.f) v = 100.f;
    return v;
}

float rcfg_odo_offset() {
    bool has; float o;
    portENTER_CRITICAL(&s_mux);
    has = s_has_odo; o = s_odo;
    portEXIT_CRITICAL(&s_mux);
    return has ? o : 0.f;
}

// 1.6.10: unabhaengig vom Schalter — der km-Stand ist ueberall der des Tachos.
float rcfg_abrp_odo(float km) { return km + rcfg_odo_offset(); }

void rcfg_status_json(JsonDocument& doc) {
    portENTER_CRITICAL(&s_mux);
    bool has_odo = s_has_odo, has_soc = s_has_soc;
    float o = s_odo, c0 = s_c[0], c1 = s_c[1];
    char t[4]; memcpy(t, s_t, sizeof(t));
    portEXIT_CRITICAL(&s_mux);
    doc["rcfg_on"] = cfg_abrp_corr();
    if (has_odo) doc["rcfg_odo"] = o;
    if (has_soc) {
        char f[64];
        snprintf(f, sizeof(f), "%.2f + %.4f × CAN%s%s", c0, c1,
                 strchr(t, 'b') ? " + Akku-Temp." : "", strchr(t, 'e') ? " + Außentemp." : "");
        doc["rcfg_soc"] = f;
    }
}
