#include "config.h"
#include "mod_didsearch.h"
#include "mod_can.h"
#include "mod_logs.h"
#include "mod_sleep.h"
#include "mod_telemetry.h"
#include "mod_rtc.h"
#include "mod_pmu.h"
#include <SPIFFS.h>

#define DS_MAX_HITS 24

struct Hit { uint16_t did; uint8_t len; uint8_t b[8]; bool match; };

static volatile bool s_running = false;
static volatile bool s_abort   = false;
static uint32_t s_req = 0, s_resp = 0;
static uint16_t s_from = 0, s_to = 0, s_pos = 0;
static int      s_target = -1;
static uint16_t s_answers = 0, s_nrc = 0, s_silent = 0;
static Hit      s_hits[DS_MAX_HITS];
static uint8_t  s_nhits = 0;
static bool     s_paused = false;
static uint8_t  s_mode = 0;              // 1 Suche, 2 Sammeln, 3 Messen, 4 Weck-Test
static uint16_t s_known = 0;             // gesammelte Kennungen (Datei)
static uint16_t s_runs  = 0;             // Messlaeufe (Datei)
static uint16_t s_total = 0, s_done = 0; // Fortschritt Sammeln/Messen

struct Range { uint32_t req, resp; uint16_t from, to; };
// Feste Liste fuers Sammeln. Bereiche, in denen die bekannten Werte liegen,
// dazu die ueblichen Messwert-Bereiche je Steuergeraet.
static const Range COLLECT[] = {
    { 0x17FC007B, 0x17FE007B, 0x0100, 0x03FF },
    { 0x17FC007B, 0x17FE007B, 0x1E00, 0x1EFF },
    { 0x17FC007B, 0x17FE007B, 0x2A00, 0x2BFF },
    { 0x17FC007B, 0x17FE007B, 0x7400, 0x74FF },
    { 0x17FC0076, 0x17FE0076, 0x0100, 0x03FF },
    { 0x17FC0076, 0x17FE0076, 0x2100, 0x21FF },
    { 0x17FC0076, 0x17FE0076, 0x2900, 0x29FF },
    { 0x714,      0x77E,      0x0100, 0x03FF },
    { 0x714,      0x77E,      0x2200, 0x22FF },
    { 0x714,      0x77E,      0x2A00, 0x2AFF },
    { 0x710,      0x77A,      0x0200, 0x02FF },
    { 0x710,      0x77A,      0x2A00, 0x2BFF },
    { 0x746,      0x7B0,      0x2600, 0x26FF },
    { 0x7E0,      0x7E8,      0x0100, 0x02FF },
};
static constexpr int COLLECT_N = sizeof(COLLECT) / sizeof(COLLECT[0]);
static int s_target_km = -1;

static uint16_t count_lines(const char* path) {
    uint16_t n = 0;
    if (!spiffs_lock(1000)) return 0;
    File f = SPIFFS.open(path, "r");
    if (f) { while (f.available()) if (f.read() == '\n') n++; f.close(); }
    spiffs_unlock();
    return n;
}

// warten, bis der Stick senden darf (Auto wach); false = abbrechen
static bool wait_awake() {
    uint32_t waited = 0;
    while (!can_tx_ok() && !s_abort && !g_shutdown && waited < 120000) {
        s_paused = true; vTaskDelay(pdMS_TO_TICKS(500)); waited += 500;
    }
    s_paused = false;
    if (waited >= 120000) syslog("CAN", "DID: abgebrochen, Auto 2 min nicht wach");
    return !s_abort && !g_shutdown && waited < 120000;
}

// Kommt der angezeigte Wert in den Bytes vor? Ein Byte, zwei Byte (big und
// little endian), auch in gaengigen Skalierungen; Toleranz +-3.
static bool matches(const uint8_t* b, int n, int target) {
    if (target < 0) return false;
    auto near = [&](float v) { return fabsf(v - target) <= 3.0f; };
    for (int i = 0; i < n; i++) {
        if (near(b[i]) || near(b[i] * 2.0f)) return true;
        if (i + 1 < n) {
            uint16_t be = (uint16_t)(b[i] << 8 | b[i + 1]), le = (uint16_t)(b[i + 1] << 8 | b[i]);
            for (uint16_t v : { be, le })
                if (near(v) || near(v / 10.0f) || near(v / 2.0f) || near(v / 4.0f)) return true;
        }
    }
    return false;
}

static void search_task(void*) {
    char m[120];
    snprintf(m, sizeof(m), "DID-Suche @%08lX 0x%04X-0x%04X%s", (unsigned long)s_req, s_from, s_to,
             s_target >= 0 ? " (Ziel " : "");
    if (s_target >= 0) { size_t k = strlen(m); snprintf(m + k, sizeof(m) - k, "%d)", s_target); }
    syslog("CAN", m);
    for (uint32_t did = s_from; did <= s_to && !s_abort && !g_shutdown; did++) {
        s_pos = (uint16_t)did;
        // nur solange das Auto wach ist; sonst warten statt Leerlauf zaehlen
        if (!wait_awake()) break;
        uint8_t req[3] = { 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
        uint8_t resp[64] = {};
        int n = can_isotp_query(s_req, s_resp, req, 3, resp, sizeof(resp), UDS_TIMEOUT_MS);
        if (n >= 4 && resp[0] == 0x62) {
            s_answers++;
            const uint8_t* d = resp + 3; int dn = n - 3;
            bool hit = matches(d, dn, s_target);
            int k = snprintf(m, sizeof(m), "%sDID 0x%04X:", hit ? "TREFFER " : "", (unsigned)did);
            for (int i = 0; i < dn && i < 16 && k < (int)sizeof(m) - 4; i++) k += snprintf(m + k, sizeof(m) - k, " %02X", d[i]);
            syslog("CAN", m);
            if (s_nhits < DS_MAX_HITS || hit) {
                int slot = s_nhits < DS_MAX_HITS ? s_nhits++ : DS_MAX_HITS - 1;
                Hit& h = s_hits[slot];
                h.did = (uint16_t)did; h.len = (uint8_t)min(dn, 8); memcpy(h.b, d, h.len); h.match = hit;
            }
        } else if (n >= 3 && resp[0] == 0x7F) {
            s_nrc++;
        } else {
            s_silent++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    snprintf(m, sizeof(m), "DID-Suche fertig: %u Antworten, %u abgelehnt, %u still",
             s_answers, s_nrc, s_silent);
    syslog("CAN", m);
    s_running = false;
    vTaskDelete(NULL);
}

// ── Sammeln ──────────────────────────────────────────────────
static uint8_t s_phase = 0;              // Messen: 1 sammelt erst, 2 liest

static void collect_total() {
    s_total = 0; s_done = 0;
    for (int k = 0; k < COLLECT_N; k++) s_total += COLLECT[k].to - COLLECT[k].from + 1;
}

// Liste neu aufbauen: jede Kennung, die antwortet, nach DID_KNOWN_FILE.
static void do_collect() {
    char m[96];
    syslog("CAN", "DID sammeln: Start");
    if (spiffs_lock(1000)) { SPIFFS.remove(DID_KNOWN_FILE); spiffs_unlock(); }
    s_known = 0;
    for (int k = 0; k < COLLECT_N && !s_abort && !g_shutdown; k++) {
        const Range& rg = COLLECT[k];
        s_req = rg.req; s_resp = rg.resp; s_from = rg.from; s_to = rg.to;
        int silent_run = 0, found = 0;
        for (uint32_t did = rg.from; did <= rg.to && !s_abort && !g_shutdown; did++) {
            s_pos = (uint16_t)did; s_done++;
            if (!wait_awake()) break;
            uint8_t req[3] = { 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
            uint8_t resp[64] = {};
            int n = can_isotp_query(rg.req, rg.resp, req, 3, resp, sizeof(resp), 150);
            if (n >= 4 && resp[0] == 0x62) {
                silent_run = 0; found++; s_answers++;
                if (spiffs_lock(1000)) {
                    File f = SPIFFS.open(DID_KNOWN_FILE, "a");
                    if (f) { f.printf("%lX %lX %04X\n", (unsigned long)rg.req, (unsigned long)rg.resp, (unsigned)did); f.close(); }
                    spiffs_unlock();
                }
                s_known++;
            } else if (n >= 3 && resp[0] == 0x7F) {
                silent_run = 0; s_nrc++;
            } else {
                s_silent++;
                if (++silent_run >= 32 && found == 0) {   // Steuergeraet antwortet hier nicht
                    s_done += rg.to - did;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        snprintf(m, sizeof(m), "DID sammeln %lX %04X-%04X: %d Kennungen", (unsigned long)rg.req, rg.from, rg.to, found);
        syslog("CAN", m);
    }
    snprintf(m, sizeof(m), "DID sammeln fertig: %u Kennungen antworten", s_known);
    syslog("CAN", m);
}

static void collect_task(void*) {
    do_collect();
    s_running = false;
    vTaskDelete(NULL);
}

bool didsearch_collect() {
    if (s_running) return false;
    s_mode = 2; s_phase = 0; s_answers = s_nrc = s_silent = 0; s_nhits = 0; s_abort = false; s_paused = false;
    collect_total();
    s_running = true;
    if (xTaskCreatePinnedToCore(collect_task, "DIDCOLL", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        s_running = false; return false;
    }
    return true;
}

// ── Messen ───────────────────────────────────────────────────
// 1.7.11: ein Ablauf. Ohne Liste sammelt der Stick zuerst. Die Antworten gehen
// Stueck fuer Stueck in eine Zwischendatei (vorher eine Zeichenkette von bis zu
// 30 KB im Speicher, die beim Anhaengen scheitern konnte; ausgewertet wurde nie
// ein Lauf) und werden erst am Ende an DID_RUNS_FILE gehaengt. Antworten, in
// denen die angezeigte Reichweite vorkommt, stehen als Treffer in der Anzeige.
#define DID_RUN_TMP "/did_run.tmp"
#define DID_LIST_MAX 640

static bool tmp_write(const String& part) {
    if (!part.length()) return true;
    if (!spiffs_lock(2000)) return false;
    File f = SPIFFS.open(DID_RUN_TMP, "a");
    bool ok = f && f.print(part) == part.length();
    if (f) f.close();
    spiffs_unlock();
    return ok;
}

// Zwischendatei an die Messlaeufe haengen
static bool tmp_commit() {
    if (!spiffs_lock(4000)) return false;
    File in = SPIFFS.open(DID_RUN_TMP, "r");
    File out = SPIFFS.open(DID_RUNS_FILE, "a");
    bool ok = in && out;
    uint8_t buf[256];
    while (ok && in.available()) {
        size_t n = in.read(buf, sizeof(buf));
        if (out.write(buf, n) != n) ok = false;
    }
    if (in) in.close();
    if (out) out.close();
    SPIFFS.remove(DID_RUN_TMP);
    spiffs_unlock();
    return ok;
}

static void measure_task(void*) {
    struct K { uint32_t req, resp; uint16_t did; };
    static K list[DID_LIST_MAX];
    char m[120];
    if (count_lines(DID_KNOWN_FILE) == 0) {        // erst die Liste
        s_phase = 1; collect_total();
        do_collect();
    }
    s_phase = 2;
    int nl = 0;
    if (!s_abort && spiffs_lock(1000)) {
        File f = SPIFFS.open(DID_KNOWN_FILE, "r");
        while (f && f.available() && nl < DID_LIST_MAX) {
            String line = f.readStringUntil('\n');
            unsigned long a, b; unsigned c;
            if (sscanf(line.c_str(), "%lX %lX %X", &a, &b, &c) == 3) list[nl++] = { (uint32_t)a, (uint32_t)b, (uint16_t)c };
        }
        if (f) f.close();
        spiffs_unlock();
    }
    s_total = nl; s_done = 0; s_answers = s_nrc = s_silent = 0; s_nhits = 0;
    if (!nl) {
        syslog("CAN", s_abort ? "DID messen: abgebrochen" : "DID messen: keine Kennung antwortet");
        s_running = false; vTaskDelete(NULL); return;
    }
    if (spiffs_lock(1000)) { SPIFFS.remove(DID_RUN_TMP); spiffs_unlock(); }
    // Kopf der Zeile: Zeit, angezeigte km, Werte des Sticks
    float soc = NAN, odo = NAN, et = NAN, bt = NAN; uint32_t age;
    telem_get_latest(TELEM_SOC, &soc, &age);
    telem_get_latest(TELEM_ODOMETER, &odo, &age);
    telem_get_latest(TELEM_EXT_TEMP, &et, &age);
    telem_get_latest(TELEM_BATT_TEMP, &bt, &age);
    char h[200];
    snprintf(h, sizeof(h), "{\"t\":%lu,\"km\":%d,\"soc\":%.1f,\"odo\":%.0f,\"et\":%.1f,\"bt\":%.1f,\"r\":{",
             (unsigned long)(rtc_unix_ms() / 1000ULL), s_target_km, isnan(soc) ? -1.f : soc, isnan(odo) ? -1.f : odo,
             isnan(et) ? -99.f : et, isnan(bt) ? -99.f : bt);
    String part; part.reserve(1600);
    part += h;
    bool first = true, ok = true;
    for (int i = 0; i < nl && !s_abort && !g_shutdown && ok; i++) {
        s_done = i + 1; s_pos = list[i].did; s_req = list[i].req;
        if (!wait_awake()) break;
        uint8_t req[3] = { 0x22, (uint8_t)(list[i].did >> 8), (uint8_t)(list[i].did & 0xFF) };
        uint8_t resp[64] = {};
        int n = can_isotp_query(list[i].req, list[i].resp, req, 3, resp, sizeof(resp), 200);
        if (n >= 4 && resp[0] == 0x62) {
            s_answers++;
            char kv[48]; snprintf(kv, sizeof(kv), "%s\"%lX:%04X\":\"", first ? "" : ",", (unsigned long)list[i].req, list[i].did);
            part += kv; first = false;
            for (int j = 3; j < n && j < 3 + 24; j++) { char b[3]; snprintf(b, sizeof(b), "%02X", resp[j]); part += b; }
            part += "\"";
            const uint8_t* d = resp + 3; int dn = n - 3;
            if (matches(d, dn, s_target_km) && s_nhits < DS_MAX_HITS) {
                Hit& hh = s_hits[s_nhits++];
                hh.did = list[i].did; hh.len = (uint8_t)min(dn, 8); memcpy(hh.b, d, hh.len); hh.match = true;
            }
        } else if (n >= 3 && resp[0] == 0x7F) s_nrc++;
        else s_silent++;
        if (part.length() > 1200) { ok = tmp_write(part); part = ""; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    // Ende des Laufs: die Reichweite kann sich waehrend der Fahrt um 1-2 km aendern
    char tail[48]; snprintf(tail, sizeof(tail), "},\"t_end\":%lu}\n", (unsigned long)(rtc_unix_ms() / 1000ULL));
    part += tail;
    bool saved = false;
    if (ok && !s_abort && !g_shutdown) saved = tmp_write(part) && tmp_commit();
    else if (spiffs_lock(1000)) { SPIFFS.remove(DID_RUN_TMP); spiffs_unlock(); }
    if (saved) s_runs++;
    if (saved) snprintf(m, sizeof(m), "DID messen fertig (Anzeige %d km): %u Werte, %u Treffer, Lauf %u gespeichert",
                        s_target_km, s_answers, s_nhits, s_runs);
    else snprintf(m, sizeof(m), "DID messen: Lauf NICHT gespeichert (%s)", s_abort ? "abgebrochen" : "Speicher voll?");
    syslog("CAN", m);
    s_running = false;
    vTaskDelete(NULL);
}

bool didsearch_measure(int target_km) {
    if (s_running || target_km < 0) return false;
    s_mode = 3; s_phase = 0; s_target_km = target_km; s_answers = s_nrc = s_silent = 0; s_nhits = 0;
    s_abort = false; s_paused = false;
    s_running = true;
    if (xTaskCreatePinnedToCore(measure_task, "DIDMEAS", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
        s_running = false; return false;
    }
    return true;
}

void didsearch_clear_runs()  { if (spiffs_lock(1000)) { SPIFFS.remove(DID_RUNS_FILE);  spiffs_unlock(); } s_runs = 0; }
void didsearch_clear_known() { if (spiffs_lock(1000)) { SPIFFS.remove(DID_KNOWN_FILE); spiffs_unlock(); } s_known = 0; }

// ── Weck-Test ────────────────────────────────────────────────
static void wake_task(void*) {
    char m[120];
    bool vbus = pmu_is_vbus_in();
    snprintf(m, sizeof(m), "Weck-Test: Spannung am Port %s", vbus ? "da" : "WEG");
    syslog("CAN", m);
    uint8_t resp[64];
    int n = -1;
    for (int k = 0; k < 3 && n < 4; k++) {          // erste Anfrage weckt ggf. erst das Gateway
        memset(resp, 0, sizeof(resp));
        n = can_wake_query(0x17FC007B, 0x17FE007B, 0x028C, resp, sizeof(resp));
        if (n < 4) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (n >= 4 && resp[0] == 0x62)
        snprintf(m, sizeof(m), "Weck-Test: Auto ANTWORTET (SoC-Rohwert %02X) — Lesen ohne Zuendung moeglich", resp[3]);
    else if (n >= 3 && resp[0] == 0x7F)
        snprintf(m, sizeof(m), "Weck-Test: Auto antwortet mit Ablehnung NRC 0x%02X", resp[2]);
    else
        snprintf(m, sizeof(m), "Weck-Test: keine Antwort (Bus schlaeft) — ohne Zuendung nichts zu holen");
    syslog("CAN", m);
    s_running = false;
    vTaskDelete(NULL);
}

bool didsearch_wake_test() {
    if (s_running) return false;
    s_mode = 4; s_running = true; s_abort = false;
    if (xTaskCreatePinnedToCore(wake_task, "WAKETEST", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        s_running = false; return false;
    }
    return true;
}

bool didsearch_start(uint32_t req_id, uint32_t resp_id, uint16_t from, uint16_t to, int target) {
    if (s_running || from > to || to - from > 0x0FFF) return false;
    s_mode = 1;
    s_req = req_id; s_resp = resp_id; s_from = from; s_to = to; s_pos = from; s_target = target;
    s_answers = s_nrc = s_silent = 0; s_nhits = 0; s_abort = false; s_paused = false;
    s_running = true;
    if (xTaskCreatePinnedToCore(search_task, "DIDSEARCH", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

void didsearch_abort() { s_abort = true; }

void didsearch_status(JsonDocument& doc) {
    static bool s_counted = false;
    if (!s_counted) { s_known = count_lines(DID_KNOWN_FILE); s_runs = count_lines(DID_RUNS_FILE); s_counted = true; }
    doc["mode"]    = s_mode;
    doc["phase"]   = s_phase;
    doc["target"]  = s_mode == 3 ? s_target_km : s_target;
    doc["known"]   = s_known;
    doc["runs"]    = s_runs;
    doc["total"]   = s_total;
    doc["done"]    = s_done;
    doc["running"] = (bool)s_running;
    doc["paused"]  = s_paused;
    doc["ecu"]     = s_req;
    doc["from"]    = s_from;
    doc["to"]      = s_to;
    doc["pos"]     = s_pos;
    doc["answers"] = s_answers;
    doc["nrc"]     = s_nrc;
    doc["silent"]  = s_silent;
    JsonArray a = doc["hits"].to<JsonArray>();
    for (int i = 0; i < s_nhits; i++) {
        JsonObject o = a.add<JsonObject>();
        char did[8]; snprintf(did, sizeof(did), "%04X", s_hits[i].did);
        char hex[26] = ""; int k = 0;
        for (int j = 0; j < s_hits[i].len; j++) k += snprintf(hex + k, sizeof(hex) - k, j ? " %02X" : "%02X", s_hits[i].b[j]);
        o["did"] = did; o["hex"] = hex; o["match"] = s_hits[i].match;
    }
}
