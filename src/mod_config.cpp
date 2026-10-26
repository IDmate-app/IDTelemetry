#include "mod_config.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Arduino.h>

#define CFG_NS "cfg"

// ── Laufzeit-Speicher (vorinitialisiert mit secrets.h Werten) ──
static char s_ap_ssid   [64]  = SECRET_AP_SSID;
static char s_ap_pass   [64]  = SECRET_AP_PASS;
static char s_sim_pin   [12]  = SECRET_PIN;
static char s_apn       [64]  = SECRET_APN;
static char s_apn_user  [32]  = SECRET_APN_USER;
static char s_apn_pass  [32]  = SECRET_APN_PASS;
static char s_tc_host   [96]  = SECRET_TRACCAR_HOST;
static char s_tc_id     [64]  = SECRET_TRACCAR_ID;
static char s_ix_host   [96]  = SECRET_INFLUX_HOST;
static char s_ix_org    [64]  = SECRET_INFLUX_ORG;
static char s_ix_bucket [64]  = SECRET_INFLUX_BUCKET;
static char s_ix_token  [256] = SECRET_INFLUX_TOKEN;
static char s_ix_device [32]  = SECRET_INFLUX_DEVICE;
static char s_mq_host  [96]  = SECRET_MQTT_HOST;
static uint16_t s_mq_port    = SECRET_MQTT_PORT;
static char s_mq_user  [64]  = SECRET_MQTT_USER;
static char s_mq_pass  [64]  = SECRET_MQTT_PASS;
static char s_mq_topic [64]  = SECRET_MQTT_TOPIC;
// AES-Key override — empty = fall back to SECRET_AES_KEY at compile time.
// Storing here lets end users configure the key via the Web UI without
// rebuilding firmware. Kept opt-in: if NVS is empty, the secrets.h key
// continues to be used, so existing sticks are not disturbed.
static char s_aes_key  [128] = "";
// WiFi STA upload — two priority slots (e.g. home WiFi + phone hotspot).
// mod_wifi_upload scans on every reconnect attempt and joins whichever
// slot's SSID is in range. Slot 1 wins ties (higher priority).
// Empty SSID = slot disabled.
static char s_sta_ssid    [64]  = "";
static char s_sta_pass    [64]  = "";
static char s_upload_url  [128] = "";
static char s_sta_ssid_2  [64]  = "";
static char s_sta_pass_2  [64]  = "";
static char s_upload_url_2[128] = "";
static bool s_ble_standby     = false;  // Default: aus
// GPS source selector: "ext" (BLITZ M10), "int" (SIM7080G), "off".
// Default ext — that's what the codebase actually uses since v1.1.
static char s_gps_src[8] = "ext";
static bool s_mod_gps         = true;   // Default: GPS vorhanden
static bool s_mod_compass     = false;  // FIXES B.17: Default aus (kein Treiber aktiv, Setting persistiert nur)
static bool s_log_can         = LOG_CAN_ENABLED_DEFAULT;
static bool s_log_ble         = LOG_BLE_ENABLED_DEFAULT;
static bool s_log_wifi        = LOG_WIFI_ENABLED_DEFAULT;
static char s_lang[8]         = "de";

static void pref_load(Preferences& p, const char* key,
                      char* dst, size_t sz, const char* def) {
    String v = p.getString(key, def);
    strncpy(dst, v.c_str(), sz - 1);
    dst[sz - 1] = '\0';
}

void cfg_init() {
    Preferences p;
    p.begin(CFG_NS, true);   // read-only
    pref_load(p, "ap_ssid",   s_ap_ssid,   sizeof(s_ap_ssid),   SECRET_AP_SSID);
    pref_load(p, "ap_pass",   s_ap_pass,   sizeof(s_ap_pass),   SECRET_AP_PASS);
    pref_load(p, "sim_pin",   s_sim_pin,   sizeof(s_sim_pin),   SECRET_PIN);
    pref_load(p, "apn",       s_apn,       sizeof(s_apn),       SECRET_APN);
    pref_load(p, "apn_user",  s_apn_user,  sizeof(s_apn_user),  SECRET_APN_USER);
    pref_load(p, "apn_pass",  s_apn_pass,  sizeof(s_apn_pass),  SECRET_APN_PASS);
    pref_load(p, "tc_host",   s_tc_host,   sizeof(s_tc_host),   SECRET_TRACCAR_HOST);
    pref_load(p, "tc_id",     s_tc_id,     sizeof(s_tc_id),     SECRET_TRACCAR_ID);
    pref_load(p, "ix_host",   s_ix_host,   sizeof(s_ix_host),   SECRET_INFLUX_HOST);
    pref_load(p, "ix_org",    s_ix_org,    sizeof(s_ix_org),    SECRET_INFLUX_ORG);
    pref_load(p, "ix_bucket", s_ix_bucket, sizeof(s_ix_bucket), SECRET_INFLUX_BUCKET);
    pref_load(p, "ix_token",  s_ix_token,  sizeof(s_ix_token),  SECRET_INFLUX_TOKEN);
    pref_load(p, "ix_device", s_ix_device, sizeof(s_ix_device), SECRET_INFLUX_DEVICE);
    pref_load(p, "mq_host",  s_mq_host,  sizeof(s_mq_host),  SECRET_MQTT_HOST);
    s_mq_port = p.getUShort("mq_port", SECRET_MQTT_PORT);
    pref_load(p, "mq_user",  s_mq_user,  sizeof(s_mq_user),  SECRET_MQTT_USER);
    pref_load(p, "mq_pass",  s_mq_pass,  sizeof(s_mq_pass),  SECRET_MQTT_PASS);
    pref_load(p, "mq_topic", s_mq_topic, sizeof(s_mq_topic), SECRET_MQTT_TOPIC);
    pref_load(p, "aes_key",  s_aes_key,  sizeof(s_aes_key),  "");
    pref_load(p, "sta_ssid",   s_sta_ssid,   sizeof(s_sta_ssid),    "");
    pref_load(p, "sta_pass",   s_sta_pass,   sizeof(s_sta_pass),    "");
    pref_load(p, "upload_url", s_upload_url, sizeof(s_upload_url),  "");
    pref_load(p, "sta_ssid2",  s_sta_ssid_2, sizeof(s_sta_ssid_2),  "");
    pref_load(p, "sta_pass2",  s_sta_pass_2, sizeof(s_sta_pass_2),  "");
    pref_load(p, "url2",       s_upload_url_2, sizeof(s_upload_url_2), "");
    pref_load(p, "gps_src",    s_gps_src,    sizeof(s_gps_src),    "ext");
    s_ble_standby = p.getBool("ble_stdby", false);
    s_mod_gps     = p.getBool("mod_gps",     true);
    s_mod_compass = p.getBool("mod_compass", false);
    s_log_can     = p.getBool("log_can", LOG_CAN_ENABLED_DEFAULT);
    s_log_ble     = p.getBool("log_ble", LOG_BLE_ENABLED_DEFAULT);
    s_log_wifi    = p.getBool("log_wifi", LOG_WIFI_ENABLED_DEFAULT);
    pref_load(p, "lang", s_lang, sizeof(s_lang), "de");
    p.end();
    Serial.println("[CFG] init OK");
    Serial.printf("[CFG] Traccar: https://%s  ID=%s\n", s_tc_host, s_tc_id);
    Serial.printf("[CFG] InfluxDB: https://%s  org=%s  bucket=%s\n", s_ix_host, s_ix_org, s_ix_bucket);
    Serial.printf("[CFG] MQTT: %s:%d  topic=%s\n", s_mq_host, s_mq_port, s_mq_topic);
}

const char* cfg_ap_ssid()       { return s_ap_ssid; }
const char* cfg_ap_pass()       { return s_ap_pass; }
const char* cfg_sim_pin()       { return s_sim_pin; }
const char* cfg_apn()           { return s_apn; }
const char* cfg_apn_user()      { return s_apn_user; }
const char* cfg_apn_pass()      { return s_apn_pass; }
const char* cfg_traccar_host()  { return s_tc_host; }
const char* cfg_traccar_id()    { return s_tc_id; }
const char* cfg_influx_host()   { return s_ix_host; }
const char* cfg_influx_org()    { return s_ix_org; }
const char* cfg_influx_bucket() { return s_ix_bucket; }
const char* cfg_influx_token()  { return s_ix_token; }
const char* cfg_influx_device() { return s_ix_device; }
const char* cfg_mqtt_host()    { return s_mq_host; }
uint16_t    cfg_mqtt_port()    { return s_mq_port; }
const char* cfg_mqtt_user()    { return s_mq_user; }
const char* cfg_mqtt_pass()    { return s_mq_pass; }
const char* cfg_mqtt_topic()   { return s_mq_topic; }
const char* cfg_aes_key()      { return s_aes_key; }
const char* cfg_sta_ssid()     { return s_sta_ssid; }
const char* cfg_sta_pass()     { return s_sta_pass; }
const char* cfg_upload_url()   { return s_upload_url; }
const char* cfg_sta_ssid_2()   { return s_sta_ssid_2; }
const char* cfg_sta_pass_2()   { return s_sta_pass_2; }
const char* cfg_upload_url_2() { return s_upload_url_2; }
const char* cfg_gps_src()      { return s_gps_src; }
bool        cfg_ble_standby()  { return s_ble_standby; }
bool        cfg_mod_gps()      { return s_mod_gps; }
bool        cfg_mod_compass()  { return s_mod_compass; }
bool        cfg_log_can()      { return s_log_can; }
bool        cfg_log_ble()      { return s_log_ble; }
bool        cfg_log_wifi()     { return s_log_wifi; }
const char* cfg_lang()         { return s_lang; }

// ── FIXES B.14: Eingabe-Validierung ──────────────────────────
static bool valid_ap_pass(const char* s)  { size_t n = s ? strlen(s) : 0; return n >= 8 && n <= 63; }
static bool valid_sim_pin(const char* s) {            // leer ODER 4-8 Ziffern
    if (!s || !*s) return true;
    size_t n = strlen(s);
    if (n < 4 || n > 8) return false;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static bool valid_aes_key(const char* s) {            // genau 64 Hex und NICHT all-zero
    if (!s || strlen(s) != 64) return false;
    bool nonzero = false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        bool hex = (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
        if (!hex) return false;
        if (c != '0') nonzero = true;
    }
    return nonzero;
}
static bool valid_upload_url(const char* s) {         // leer ODER bekanntes Schema
    if (!s || !*s) return true;
    return strncmp(s,"http://",7)==0 || strncmp(s,"https://",8)==0 ||
           strncmp(s,"mqtt://",7)==0 || strncmp(s,"mqtts://",8)==0;
}

bool cfg_save_json(const uint8_t* body, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, body, len)) return false;

    Preferences p;
    p.begin(CFG_NS, false);   // read-write

    auto save = [&](const char* key, char* dst, size_t sz, JsonVariant v) {
        if (v.isNull()) return;
        const char* s = v.as<const char*>();
        if (!s) return;
        strncpy(dst, s, sz - 1);
        dst[sz - 1] = '\0';
        p.putString(key, dst);
    };

    // Wie save(), aber nur wenn validate(s) true ist — sonst alten Wert behalten + warnen.
    auto save_valid = [&](const char* key, char* dst, size_t sz, JsonVariant v,
                          bool (*validate)(const char*), const char* why) {
        if (v.isNull()) return;
        const char* s = v.as<const char*>();
        if (!s) return;
        if (!validate(s)) { Serial.printf("[CFG] %s ignoriert (%s) — alter Wert bleibt\n", key, why); return; }
        strncpy(dst, s, sz - 1);
        dst[sz - 1] = '\0';
        p.putString(key, dst);
    };

    save("ap_ssid",   s_ap_ssid,   sizeof(s_ap_ssid),   doc["ap_ssid"]);
    save_valid("ap_pass", s_ap_pass, sizeof(s_ap_pass), doc["ap_pass"], valid_ap_pass, "8-63 Zeichen, sonst offenes Netz");
    save_valid("sim_pin", s_sim_pin, sizeof(s_sim_pin), doc["sim_pin"], valid_sim_pin, "4-8 Ziffern oder leer, PUK-Sperr-Risiko");
    save("apn",       s_apn,       sizeof(s_apn),       doc["apn"]);
    save("apn_user",  s_apn_user,  sizeof(s_apn_user),  doc["apn_user"]);
    save("apn_pass",  s_apn_pass,  sizeof(s_apn_pass),  doc["apn_pass"]);
    save("tc_host",   s_tc_host,   sizeof(s_tc_host),   doc["tc_host"]);
    save("tc_id",     s_tc_id,     sizeof(s_tc_id),     doc["tc_id"]);
    save("ix_host",   s_ix_host,   sizeof(s_ix_host),   doc["ix_host"]);
    save("ix_org",    s_ix_org,    sizeof(s_ix_org),    doc["ix_org"]);
    save("ix_bucket", s_ix_bucket, sizeof(s_ix_bucket), doc["ix_bucket"]);
    save("ix_token",  s_ix_token,  sizeof(s_ix_token),  doc["ix_token"]);
    save("ix_device", s_ix_device, sizeof(s_ix_device), doc["ix_device"]);
    save("mq_host",   s_mq_host,  sizeof(s_mq_host),  doc["mq_host"]);
    save("mq_user",   s_mq_user,  sizeof(s_mq_user),  doc["mq_user"]);
    save("mq_pass",   s_mq_pass,  sizeof(s_mq_pass),  doc["mq_pass"]);
    save("mq_topic",  s_mq_topic, sizeof(s_mq_topic), doc["mq_topic"]);
    // FIXES B.14: AES-Key streng prüfen. Leer = bewusst Built-in SECRET_AES_KEY.
    // Ungültig (≠64 Hex oder all-zero) → NICHT speichern, auf leer/Built-in
    // zurückfallen + warnen. Sonst verschlüsselt der Stick still mit Müll-Key
    // und der Server kann NIE entschlüsseln (stiller Totalverlust).
    if (!doc["aes_key"].isNull()) {
        const char* s = doc["aes_key"].as<const char*>();
        if (s && s[0] == '\0') {
            s_aes_key[0] = '\0'; p.putString("aes_key", "");
        } else if (valid_aes_key(s)) {
            strncpy(s_aes_key, s, sizeof(s_aes_key) - 1); s_aes_key[sizeof(s_aes_key) - 1] = '\0';
            p.putString("aes_key", s_aes_key);
        } else {
            s_aes_key[0] = '\0'; p.putString("aes_key", "");
            Serial.println("[CFG] WARN aes_key ungültig (≠64 Hex oder all-zero) → Fallback auf SECRET_AES_KEY");
        }
    }
    save("sta_ssid",   s_sta_ssid,   sizeof(s_sta_ssid),   doc["sta_ssid"]);
    save("sta_pass",   s_sta_pass,   sizeof(s_sta_pass),   doc["sta_pass"]);
    save_valid("upload_url", s_upload_url, sizeof(s_upload_url), doc["upload_url"], valid_upload_url, "Schema http(s)/mqtt(s):// oder leer");
    save("sta_ssid2",  s_sta_ssid_2, sizeof(s_sta_ssid_2), doc["sta_ssid_2"]);
    save("sta_pass2",  s_sta_pass_2, sizeof(s_sta_pass_2), doc["sta_pass_2"]);
    save_valid("url2", s_upload_url_2, sizeof(s_upload_url_2), doc["upload_url_2"], valid_upload_url, "Schema http(s)/mqtt(s):// oder leer");
    save("gps_src",    s_gps_src,      sizeof(s_gps_src),      doc["gps_src"]);
    // legacy: cfg_mod_gps() boolean is derived from gps_src for back-compat
    if (!doc["gps_src"].isNull()) {
        const char* src = doc["gps_src"].as<const char*>();
        if (src) {
            s_mod_gps = (strcmp(src, "int") == 0);
            p.putBool("mod_gps", s_mod_gps);
        }
    }
    if (!doc["mq_port"].isNull()) {
        uint32_t port = doc["mq_port"].as<uint32_t>();   // FIXES B.14: 0 verbieten
        if (port >= 1 && port <= 65535) {
            s_mq_port = (uint16_t)port;
            p.putUShort("mq_port", s_mq_port);
        } else {
            Serial.println("[CFG] mq_port ignoriert (muss 1-65535) — alter Wert bleibt");
        }
    }
    save("lang",      s_lang,      sizeof(s_lang),      doc["lang"]);
    if (!doc["ble_standby"].isNull()) {
        s_ble_standby = doc["ble_standby"].as<bool>();
        p.putBool("ble_stdby", s_ble_standby);
    }
    if (!doc["mod_gps"].isNull()) {
        s_mod_gps = doc["mod_gps"].as<bool>();
        p.putBool("mod_gps", s_mod_gps);
    }
    if (!doc["mod_compass"].isNull()) {     // FIXES B.17: wurde gesendet, aber nie gespeichert
        s_mod_compass = doc["mod_compass"].as<bool>();
        p.putBool("mod_compass", s_mod_compass);
    }
    if (!doc["log_can"].isNull()) {
        s_log_can = doc["log_can"].as<bool>();
        p.putBool("log_can", s_log_can);
    }
    if (!doc["log_ble"].isNull()) {
        s_log_ble = doc["log_ble"].as<bool>();
        p.putBool("log_ble", s_log_ble);
    }
    if (!doc["log_wifi"].isNull()) {
        s_log_wifi = doc["log_wifi"].as<bool>();
        p.putBool("log_wifi", s_log_wifi);
    }
    p.end();

    Serial.println("[CFG] gespeichert");
    return true;
}

const char* cfg_to_json() {
    // FIXES B.15: war 1200 — Summe der Feld-Maxima (ix_token, aes_key,
    // upload_url ×2, …) + Keys/Quotes > 1200 → bei Vollkonfig schnitt
    // serializeJson() das JSON still ab → /api/config kaputt, i18n.js failt
    // auf ALLEN Seiten. Großzügig dimensioniert (static, RAM unkritisch).
    static char buf[3072];
    JsonDocument doc;
    doc["ap_ssid"]   = s_ap_ssid;
    doc["ap_pass"]   = s_ap_pass;
    doc["sim_pin"]   = s_sim_pin;
    doc["apn"]       = s_apn;
    doc["apn_user"]  = s_apn_user;
    doc["apn_pass"]  = s_apn_pass;
    doc["tc_host"]   = s_tc_host;
    doc["tc_id"]     = s_tc_id;
    doc["ix_host"]   = s_ix_host;
    doc["ix_org"]    = s_ix_org;
    doc["ix_bucket"] = s_ix_bucket;
    doc["ix_token"]  = s_ix_token;
    doc["ix_device"]    = s_ix_device;
    doc["mq_host"]      = s_mq_host;
    doc["mq_port"]      = s_mq_port;
    doc["mq_user"]      = s_mq_user;
    doc["mq_pass"]      = s_mq_pass;
    doc["mq_topic"]     = s_mq_topic;
    doc["aes_key"]      = s_aes_key;
    doc["sta_ssid"]     = s_sta_ssid;
    doc["sta_pass"]     = s_sta_pass;
    doc["upload_url"]   = s_upload_url;
    doc["sta_ssid_2"]   = s_sta_ssid_2;
    doc["sta_pass_2"]   = s_sta_pass_2;
    doc["upload_url_2"] = s_upload_url_2;
    doc["gps_src"]      = s_gps_src;
    doc["ble_standby"]  = s_ble_standby;
    doc["mod_gps"]      = s_mod_gps;
    doc["mod_compass"]  = s_mod_compass;
    doc["log_can"]      = s_log_can;
    doc["log_ble"]      = s_log_ble;
    doc["log_wifi"]     = s_log_wifi;
    doc["lang"]         = s_lang;
    serializeJson(doc, buf, sizeof(buf));
    return buf;
}
