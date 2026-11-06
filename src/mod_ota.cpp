#include "mod_ota.h"
#include "config.h"
#include "mod_config.h"
#include "mod_logs.h"
#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <string.h>

#define OTA_NS          "ota"
#define OTA_MAX_FAILS   3
#define OTA_STABLE_MS   120000UL
#define CHIP_ID_ESP32S3 0x0009

static const char* s_err = "";
static bool        s_active = false;
static bool        s_ok     = false;

static bool image_valid(const esp_partition_t* p) {
    esp_app_desc_t d;
    return p && esp_ota_get_partition_description(p, &d) == ESP_OK;
}

static const esp_partition_t* other_slot() {
    return esp_ota_get_next_update_partition(NULL);
}

bool ota_possible() { return other_slot() != nullptr; }

void ota_boot_guard() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* other = other_slot();
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;

    Preferences p;
    if (!p.begin(OTA_NS, false)) return;
    uint8_t fails = p.getUChar("fails", 0);
    bool fresh = p.getBool("fresh", false);
    if (crash) fails++;
    p.putUChar("fails", fails);

    if (fresh && fails >= OTA_MAX_FAILS && image_valid(other)) {
        Serial.printf("[OTA] %u Abstuerze der neuen Firmware -> zurueck auf %s\n", fails, other->label);
        p.putUChar("fails", 0);
        p.putBool("fresh", false);
        p.putBool("rolled", true);
        p.end();
        if (esp_ota_set_boot_partition(other) == ESP_OK) {
            delay(100);
            esp_restart();
        }
        return;
    }
    // Merken, welcher Build in welchem Platz laeuft (fuer "zurueck zu ...")
    if (run) {
        char key[12];
        snprintf(key, sizeof(key), "b_%s", run->label);
        if (p.getString(key, "") != FW_BUILD) p.putString(key, FW_BUILD);
    }
    p.end();
}

void ota_stable_tick() {
    static bool done = false;
    if (done || millis() < OTA_STABLE_MS) return;
    done = true;
    Preferences p;
    if (!p.begin(OTA_NS, false)) return;
    if (p.getUChar("fails", 0) != 0) p.putUChar("fails", 0);
    if (p.getBool("fresh", false)) {
        p.putBool("fresh", false);
        p.end();
        syslog("OTA", "Neue Firmware laeuft stabil (2 min) — bewaehrt");
        return;
    }
    p.end();
}

void ota_boot_log() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    Preferences p;
    if (!p.begin(OTA_NS, false)) return;
    bool rolled = p.getBool("rolled", false);
    if (rolled) p.putBool("rolled", false);
    p.end();
    char m[96];
    snprintf(m, sizeof(m), "Platz %s · zweiter Platz %s", run ? run->label : "?",
             ota_possible() ? "vorhanden (Web-Update moeglich)" : "fehlt (nur USB)");
    syslog("OTA", m);
    if (rolled) syslog("OTA", "RUECKFALL: neue Firmware ist 3x abgestuerzt, vorherige laeuft wieder");
}

void ota_status_json(JsonDocument& doc) {
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* other = other_slot();
    doc["ota_possible"] = other != nullptr;
    doc["ota_slot"] = run ? run->label : "";
    if (other && image_valid(other)) {
        Preferences p;
        if (p.begin(OTA_NS, true)) {
            char key[12];
            snprintf(key, sizeof(key), "b_%s", other->label);
            String b = p.getString(key, "");
            p.end();
            doc["ota_other_build"] = b.length() ? b : String("?");
        }
    }
}

static bool pw_ok(const char* pw) {
    const char* ap = cfg_ap_pass();
    return !ap || !ap[0] || (pw && strcmp(pw, ap) == 0);
}

bool ota_upload_begin(const char* filename, const char* pw, const uint8_t* first, size_t len) {
    s_err = "";
    s_active = false;
    s_ok = false;
    if (!ota_possible())        { s_err = "noslot"; return false; }
    if (!pw_ok(pw))             { s_err = "pw"; return false; }
    // Variante am Dateinamen (Release-Assets heissen idtelemetry-full-/-lite-...)
    bool full = strcmp(FW_VARIANT, "full") == 0;
    if (filename && ((full && strstr(filename, "lite")) || (!full && strstr(filename, "full")))) {
        s_err = "variant"; return false;
    }
    // ESP-App-Image fuer ESP32-S3: Magic 0xE9, Chip-ID an Byte 12/13
    if (len < 16 || first[0] != 0xE9 || (uint16_t)(first[12] | (first[13] << 8)) != CHIP_ID_ESP32S3) {
        s_err = "image"; return false;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { s_err = "begin"; return false; }
    s_active = true;
    syslog("OTA", filename && filename[0] ? filename : "Upload gestartet");
    return true;
}

bool ota_upload_write(const uint8_t* data, size_t len) {
    if (!s_active) return false;
    if (Update.write((uint8_t*)data, len) != len) { s_err = "write"; s_active = false; Update.abort(); return false; }
    return true;
}

bool ota_upload_end() {
    if (!s_active) return false;
    s_active = false;
    if (!Update.end(true)) { s_err = "verify"; return false; }   // prueft Image + SHA, setzt Boot-Platz
    s_ok = true;
    Preferences p;
    if (p.begin(OTA_NS, false)) {
        p.putBool("fresh", true);
        p.putUChar("fails", 0);
        p.end();
    }
    syslog("OTA", "Upload OK, Image geprueft — Neustart in die neue Firmware");
    return true;
}

const char* ota_last_error() { return s_err; }
bool ota_upload_ok() { return s_ok; }

bool ota_rollback(const char* pw, const char** err) {
    const esp_partition_t* other = other_slot();
    if (!pw_ok(pw))          { *err = "pw"; return false; }
    if (!image_valid(other)) { *err = "noimage"; return false; }
    if (esp_ota_set_boot_partition(other) != ESP_OK) { *err = "set"; return false; }
    Preferences p;
    if (p.begin(OTA_NS, false)) { p.putBool("fresh", false); p.putUChar("fails", 0); p.end(); }
    syslog("OTA", "Zurueck zur vorherigen Firmware — Neustart");
    *err = "";
    return true;
}
