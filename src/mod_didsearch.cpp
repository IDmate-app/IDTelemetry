#include "config.h"
#include "mod_didsearch.h"
#include "mod_can.h"
#include "mod_logs.h"
#include "mod_sleep.h"

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
        uint32_t waited = 0;
        while (!can_tx_ok() && !s_abort && !g_shutdown && waited < 120000) {
            s_paused = true; vTaskDelay(pdMS_TO_TICKS(500)); waited += 500;
        }
        s_paused = false;
        if (waited >= 120000) { syslog("CAN", "DID-Suche abgebrochen: Auto 2 min nicht wach"); break; }
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

bool didsearch_start(uint32_t req_id, uint32_t resp_id, uint16_t from, uint16_t to, int target) {
    if (s_running || from > to || to - from > 0x0FFF) return false;
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
    doc["running"] = (bool)s_running;
    doc["paused"]  = s_paused;
    doc["ecu"]     = s_req;
    doc["from"]    = s_from;
    doc["to"]      = s_to;
    doc["pos"]     = s_pos;
    doc["answers"] = s_answers;
    doc["nrc"]     = s_nrc;
    doc["silent"]  = s_silent;
    doc["target"]  = s_target;
    JsonArray a = doc["hits"].to<JsonArray>();
    for (int i = 0; i < s_nhits; i++) {
        JsonObject o = a.add<JsonObject>();
        char did[8]; snprintf(did, sizeof(did), "%04X", s_hits[i].did);
        char hex[26] = ""; int k = 0;
        for (int j = 0; j < s_hits[i].len; j++) k += snprintf(hex + k, sizeof(hex) - k, j ? " %02X" : "%02X", s_hits[i].b[j]);
        o["did"] = did; o["hex"] = hex; o["match"] = s_hits[i].match;
    }
}
