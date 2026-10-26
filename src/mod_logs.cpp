#include "mod_logs.h"
#include "mod_config.h"
#include "mod_rtc.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "esp_system.h"
#include <Arduino.h>

// ============================================================
//  mod_logs - Zentrales Log-Modul
//  /scan.log, /elm.log, /ble.log, /sys.log
// ============================================================

// ============================================================
//  RAM Ring-Buffer (SCAN, Live-Anzeige im Browser)
// ============================================================

LogEntry* log_buf = nullptr;  // PSRAM, allokiert in logs_init()
uint16_t  log_count = 0;          // Gesamtzahl (kann > MAX_LOG_ENTRIES sein)
static uint16_t log_head = 0;     // nächster Schreibindex (Ring)

// SPIFFS ist NICHT thread-safe. Alle Zugriffe (open/read/write/rename/remove)
// müssen gegen concurrent access geschützt werden — sonst Heap-Korruption.
// Recursive-Mutex: ermöglicht Nesting (z.B. spiffs_q_init → syslog → spiffs_append_ring).
static SemaphoreHandle_t s_spiffs_mtx = nullptr;

bool spiffs_lock(uint32_t timeout_ms) {
    if (!s_spiffs_mtx) return true;  // vor logs_init() — Single-Task-Phase
    return xSemaphoreTakeRecursive(s_spiffs_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void spiffs_unlock() {
    if (s_spiffs_mtx) xSemaphoreGiveRecursive(s_spiffs_mtx);
}

// ============================================================
//  SPIFFS Hilfsfunktion — Zeile anhängen, Ringpuffer
// ============================================================

static void spiffs_append_ring(const char* path, const char* line,
                                size_t max_bytes) {
    if (!spiffs_lock(200)) return;  // Timeout → Log-Zeile verwerfen statt Race
    if (SPIFFS.exists(path)) {
        File f = SPIFFS.open(path, "r");
        size_t sz = f ? f.size() : 0;
        f.close();

        if (sz + strlen(line) > max_bytes) {
            // Hintere Hälfte behalten — Chunk-basiert, kein Heap
            File src = SPIFFS.open(path, "r");
            if (src) {
                src.seek(sz / 2);
                // Angefangene Zeile überspringen
                while (src.available()) { if (src.read() == '\n') break; }

                File tmp = SPIFFS.open("/tmp.log", "w");
                if (tmp) {
                    uint8_t buf[256];
                    while (src.available()) {
                        int n = src.read(buf, sizeof(buf));
                        if (n > 0) tmp.write(buf, n);
                    }
                    tmp.close();
                }
                src.close();
                SPIFFS.remove(path);
                SPIFFS.rename("/tmp.log", path);
            }
        }
    }

    File f = SPIFFS.open(path, "a");
    if (f) { f.print(line); f.close(); }
    spiffs_unlock();
}

// ============================================================
//  Init
// ============================================================

void logs_init() {
    // SPIFFS-Mutex erzeugen BEVOR irgendein Task syslog() aufruft
    if (!s_spiffs_mtx) s_spiffs_mtx = xSemaphoreCreateRecursiveMutex();

    // Log-Buffer in PSRAM allokieren (spart ~13 KB internes RAM)
    log_buf = (LogEntry*)ps_calloc(MAX_LOG_ENTRIES, sizeof(LogEntry));
    if (!log_buf) {
        log_buf = (LogEntry*)calloc(MAX_LOG_ENTRIES, sizeof(LogEntry));
    }
    // SPIFFS wird in mod_web.cpp initialisiert
    Serial.printf("[LOGS] SPIFFS: %s / %s / %s (max %dKB)  %s (max %dKB)\n",
                  SPIFFS_SCAN_LOG, SPIFFS_ELM_LOG,
                  SPIFFS_BLE_LOG, SPIFFS_BLE_LOG_MAX_KB,
                  SPIFFS_SYS_LOG, SPIFFS_SYS_LOG_KB);

    esp_reset_reason_t reason = esp_reset_reason();
    const char* rstr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:   rstr = "POWER_ON";    break;
        case ESP_RST_SW:        rstr = "SW_RESET";    break;
        case ESP_RST_PANIC:     rstr = "PANIC/CRASH"; break;
        case ESP_RST_INT_WDT:   rstr = "WDT_INT";     break;
        case ESP_RST_TASK_WDT:  rstr = "WDT_TASK";    break;
        case ESP_RST_WDT:       rstr = "WDT";         break;
        case ESP_RST_DEEPSLEEP: rstr = "DEEPSLEEP";   break;
        case ESP_RST_BROWNOUT:  rstr = "BROWNOUT";    break;
        default: break;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Neustart  Grund: %s", rstr);
    syslog("BOOT", msg);
}

// ============================================================
//  CAN + ELM Log
// ============================================================

static const char* can_log_path(uint8_t src) {
    return (src == LOG_SRC_ELM) ? SPIFFS_ELM_LOG : SPIFFS_SCAN_LOG;
}

void log_add(bool tx, uint32_t id, uint8_t* d, uint8_t len,
             const char* info, uint8_t src) {
    if (!logMutex) return;
    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;

    // RAM Ring-Buffer (nur SCAN für Live-Anzeige)
    if (src == LOG_SRC_SCAN) {
        LogEntry& e = log_buf[log_head];
        e.timestamp = rtc_now_ms_of_day();
        e.is_tx     = tx;
        e.can_id    = id;
        e.len       = min(len, (uint8_t)8);
        memcpy(e.data, d, e.len);
        strncpy(e.info, info ? info : "", LOG_INFO_LEN - 1);
        e.info[LOG_INFO_LEN - 1] = '\0';

        log_head = (log_head + 1) % MAX_LOG_ENTRIES;
        if (log_count < MAX_LOG_ENTRIES) log_count++;
    }

    xSemaphoreGive(logMutex);

    // Hex-String (für SPIFFS + WebSocket)
    char hex[28] = "";
    if (cfg_log_can() || ws.count() > 0) {
        for (int i = 0; i < min(len, (uint8_t)8); i++)
            sprintf(hex + strlen(hex), "%02X ", d[i]);
    }

    // SPIFFS Zeile bauen (nur wenn CAN-Log aktiviert)
    if (cfg_log_can()) {
        String ts = rtc_now_str();
        char line[160];
        snprintf(line, sizeof(line), "%s | %s | %03X | %-23s | %s\r\n",
                 ts.c_str(), tx ? "TX" : "RX", id, hex, info ? info : "");
        spiffs_append_ring(can_log_path(src), line,
                           (size_t)SPIFFS_LOG_MAX_KB * 1024);
    }

    // WebSocket Broadcast (nur wenn Client verbunden — spart CPU+Heap)
    if (ws.count() > 0) {
        JsonDocument doc;
        doc["type"] = "log";
        JsonObject entry = doc["e"].to<JsonObject>();
        entry["t"]   = millis();
        entry["tx"]  = tx;
        char idstr[10]; sprintf(idstr, "%03X", id);
        entry["id"]  = idstr;
        entry["hex"] = hex;
        entry["dec"] = info ? info : "";
        entry["src"] = src;
        String out; serializeJson(doc, out);
        ws.textAll(out);
    }
}

void log_clear(uint8_t src) {
    if (src == LOG_SRC_SCAN) {
        if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            log_count = 0;
            log_head  = 0;
            xSemaphoreGive(logMutex);
        }
    }
    const char* path = can_log_path(src);
    if (spiffs_lock()) {
        if (SPIFFS.exists(path)) SPIFFS.remove(path);
        spiffs_unlock();
    }
    Serial.printf("[LOGS] %s gelöscht\n", path);
}

String log_to_txt(uint8_t src) {
    const char* path = can_log_path(src);
    String out = "Telemetry Stick Log\r\n";
    out += (src == LOG_SRC_ELM) ? "Quelle: ELM327/ABRP\r\n" : "Quelle: Web-Scanner\r\n";
    out += "Uptime: " + String(millis() / 1000) + "s\r\n";
    out += "========================================================\r\n";
    out += "Zeit(ms)  | Dir | ID    | Bytes                   | Info\r\n";
    out += "--------------------------------------------------------\r\n";

    if (!spiffs_lock(1000)) { out += "(busy)\r\n"; return out; }
    if (!SPIFFS.exists(path)) { spiffs_unlock(); out += "(leer)\r\n"; return out; }

    File f = SPIFFS.open(path, "r");
    if (f) {
        while (f.available()) out += f.readStringUntil('\n') + "\n";
        f.close();
    }
    spiffs_unlock();
    return out;
}

// ============================================================
//  WebSocket Scan-Status
// ============================================================

void ws_scan_status(const char* msg, uint16_t step, uint16_t total) {
    if (ws.count() == 0) return;
    JsonDocument doc;
    doc["type"]  = "scan";
    doc["msg"]   = msg;
    doc["step"]  = step;
    doc["total"] = total;
    doc["run"]   = scan_running;
    String out; serializeJson(doc, out);
    ws.textAll(out);
}

// ============================================================
//  BLE Log
// ============================================================

void log_ble_snapshot(const char* json_devices) {
    if (!cfg_log_ble()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json_devices) != DeserializationError::Ok) return;

    JsonArray arr = doc.as<JsonArray>();
    uint32_t now = millis();

    char hdr[80];
    snprintf(hdr, sizeof(hdr),
             "--- %9lums ---\r\n", now);
    spiffs_append_ring(SPIFFS_BLE_LOG, hdr,
                       (size_t)SPIFFS_BLE_LOG_MAX_KB * 1024);

    for (JsonObject item : arr) {
        if (strcmp(item["type"], "device") != 0) continue;
        const char* mac  = item["mac"]     | "??:??:??:??:??:??";
        const char* name = item["name"]    | "Unknown";
        int  rssi        = item["rssi"]    | -999;
        int  age         = item["age_s"]   | 0;
        const char* dist = item["dist"]    | "?";
        char line[120];
        snprintf(line, sizeof(line),
                 "  %s | %-20s | %4d dBm | %-4s | %3ds\r\n",
                 mac, name, rssi, dist, age);
        spiffs_append_ring(SPIFFS_BLE_LOG, line,
                           (size_t)SPIFFS_BLE_LOG_MAX_KB * 1024);
    }
}

String log_ble_to_txt() {
    String out = "Telemetry Stick - BLE Log\r\n";
    out += "Intervall: 10s\r\n";
    out += "========================================\r\n";
    if (!spiffs_lock(1000)) { out += "(busy)\r\n"; return out; }
    if (!SPIFFS.exists(SPIFFS_BLE_LOG)) { spiffs_unlock(); out += "(leer)\r\n"; return out; }
    File f = SPIFFS.open(SPIFFS_BLE_LOG, "r");
    if (f) { while (f.available()) out += f.readStringUntil('\n') + "\n"; f.close(); }
    spiffs_unlock();
    return out;
}

void log_ble_clear() {
    if (spiffs_lock()) {
        if (SPIFFS.exists(SPIFFS_BLE_LOG)) SPIFFS.remove(SPIFFS_BLE_LOG);
        spiffs_unlock();
    }
    Serial.println("[LOGS] BLE Log gelöscht");
}

// ============================================================
//  System Log
// ============================================================

const char* syslog_timestr() {
    static char buf[16];
    String s = rtc_now_str();
    strncpy(buf, s.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

void syslog(const char* category, const char* msg) {
    // Serial immer zuerst — unabhängig von SPIFFS-Zustand
    char line[140];
    snprintf(line, sizeof(line), "[%s]\t%-8s\t%s\n",
             syslog_timestr(), category, msg);
    Serial.print(line);

    // SPIFFS — gemeinsame Ring-Truncation
    spiffs_append_ring(SPIFFS_SYS_LOG, line,
                       (size_t)SPIFFS_SYS_LOG_KB * 1024);
}
