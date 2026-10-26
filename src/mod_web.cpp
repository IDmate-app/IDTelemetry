#include "config.h"
#include "mod_web.h"
#include "mod_can.h"
#include "mod_logs.h"
#include "mod_rtc.h"
#include "mod_gyro.h"
#include "mod_pmu.h"
#include "mod_modem.h"
#include "mod_headers.h"
#include "mod_telemetry.h"
#include "mod_mqtt.h"
#include "mod_wifi_upload.h"
#include "mod_config.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Update.h>

// ── Eingebettete Web-Assets ─────────────────────────────────
// Linker-Symbole von board_build.embed_txtfiles in platformio.ini.
// Liegen im App-Binary (Flash), nicht in SPIFFS → ueberleben SPIFFS-Korruption.
extern const uint8_t debug_html_start[]  asm("_binary_data_debug_html_start");
extern const uint8_t debug_html_end[]    asm("_binary_data_debug_html_end");
extern const uint8_t config_html_start[] asm("_binary_data_config_html_start");
extern const uint8_t config_html_end[]   asm("_binary_data_config_html_end");
extern const uint8_t daten_html_start[]  asm("_binary_data_daten_html_start");
extern const uint8_t daten_html_end[]    asm("_binary_data_daten_html_end");
extern const uint8_t common_js_start[]   asm("_binary_data_common_js_start");
extern const uint8_t common_js_end[]     asm("_binary_data_common_js_end");
extern const uint8_t i18n_js_start[]     asm("_binary_data_i18n_js_start");
extern const uint8_t i18n_js_end[]       asm("_binary_data_i18n_js_end");
extern const uint8_t style_css_start[]   asm("_binary_data_style_css_start");
extern const uint8_t style_css_end[]     asm("_binary_data_style_css_end");
extern const uint8_t de_json_start[]     asm("_binary_data_lang_de_json_start");
extern const uint8_t de_json_end[]       asm("_binary_data_lang_de_json_end");
extern const uint8_t en_json_start[]     asm("_binary_data_lang_en_json_start");
extern const uint8_t en_json_end[]       asm("_binary_data_lang_en_json_end");

static void send_embedded(AsyncWebServerRequest* r, const uint8_t* start,
                          const uint8_t* end, const char* mime, bool html = false) {
    // EMBED_TXTFILES (ESP-IDF) appends a NUL terminator that's included in the
    // (end - start) span. Strip it — JS / JSON parsers reject the trailing \0
    // ("Invalid or unexpected token"), HTML mostly tolerates it but it's still
    // wrong content-length. Be defensive: strip any trailing NULs.
    size_t len = (size_t)(end - start);
    while (len > 0 && start[len - 1] == 0) len--;
    auto* resp = r->beginResponse_P(200, mime, start, len);
    if (html) headers_apply(resp);
    r->send(resp);
}

static void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* c,
                        AwsEventType t, void*, uint8_t*, size_t) {
    if (t == WS_EVT_CONNECT) {
        { char m[40]; snprintf(m, sizeof(m), "WebSocket #%u verbunden", c->id()); syslog("CLIENT", m); }
        JsonDocument doc;
        doc["type"]   = "status";
        doc["uptime"] = millis();
        doc["can_ok"] = can_running;
        String out; serializeJson(doc, out);
        c->text(out);
    } else if (t == WS_EVT_DISCONNECT) {
        { char m[40]; snprintf(m, sizeof(m), "WebSocket #%u getrennt", c->id()); syslog("CLIENT", m); }
    }
}

void ws_broadcast_json(const char* json) { if (ws.count() > 0) ws.textAll(json); }

static bool     s_ap_active    = true;
static bool     s_had_client   = false;
static uint32_t s_ap_start_ms  = 0;

void web_init() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[WEB] SPIFFS Fehler!");
        return;
    }
    WiFi.softAP(cfg_ap_ssid(), cfg_ap_pass());
    s_ap_start_ms = millis();

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->redirect("/daten");
    });

    // ── Captive-Portal-Antwort ─────────────────────────────────
    // Phones probe known URLs to detect "do I have internet on this WiFi?".
    // We answer with a tiny landing page that links to the config wizard —
    // iOS / Android pops a "captive portal" sheet with this content so the
    // first-time user can jump straight into setup instead of hunting for
    // the IP address.
    //
    // For sticks that are already configured this is harmless: the user just
    // ignores the popup and uses the network normally.
    auto captive_landing = [](AsyncWebServerRequest* r) {
        r->send(200, "text/html",
            "<!DOCTYPE html><html><head>"
            "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<title>IDTelemetry — Setup</title></head>"
            "<body style=\"font-family:-apple-system,sans-serif;max-width:480px;"
            "margin:0 auto;padding:2em 1em;color:#1F2328\">"
            "<h2 style=\"text-align:center\">IDTelemetry</h2>"

            "<p style=\"text-align:center;color:#57606a\">"
            "<strong>EN:</strong> First time here? Set up WiFi upload + backend so telemetry flows.<br>"
            "<strong>DE:</strong> Erstes Mal hier? Richte WiFi-Upload + Backend ein, damit Telemetrie fließt."
            "</p>"

            "<p style=\"text-align:center;margin:1.5em 0\">"
            "<a href=\"http://192.168.4.1/config\" "
            "style=\"display:inline-block;padding:.7em 1.4em;background:#0969da;color:#fff;"
            "text-decoration:none;border-radius:6px;font-weight:600\">Open Setup · Einrichten</a></p>"

            "<ul style=\"font-size:13px;color:#57606a;line-height:1.6;padding-left:1.2em\">"
            "<li><strong>WiFi Upload (STA)</strong> — home WiFi + endpoint URL / Heim-WLAN + Endpoint-URL</li>"
            "<li><strong>MQTT Broker</strong> — host, port, topic, AES key / Host, Port, Topic, AES-Key</li>"
            "<li><strong>SIM / APN</strong> — LTE-M variant only / nur LTE-M-Variante</li>"
            "</ul>"

            "<p style=\"text-align:center;margin-top:1.5em\">"
            "<a href=\"http://192.168.4.1/daten\" "
            "style=\"color:#0969da\">→ Skip to live dashboard / Direkt zum Dashboard</a></p>"
            "</body></html>");
    };
    server.on("/hotspot-detect.html",          HTTP_GET, captive_landing);  // iOS
    server.on("/library/test/success.html",    HTTP_GET, captive_landing);  // iOS alt
    server.on("/success.txt",                  HTTP_GET, captive_landing);  // iOS neu
    server.on("/generate_204",                 HTTP_GET, captive_landing);  // Android
    server.on("/ncsi.txt",                     HTTP_GET, captive_landing);  // Windows
    server.on("/connecttest.txt",              HTTP_GET, captive_landing);  // Windows neu

    server.on("/send", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            deserializeJson(doc, data, len);
            String id_s  = doc["id"]       | "7DF";
            String hex_s = doc["hex"]      | "02 01 00 00 00 00 00 00";
            String rx_s  = doc["rxFilter"] | "0";
            uint32_t tx_id   = strtoul(id_s.c_str(), nullptr, 16);
            uint32_t rx_filt = strtoul(rx_s.c_str(), nullptr, 16);
            uint8_t  bytes[8] = {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC};
            uint8_t  cnt = 0;
            char buf[64]; strncpy(buf, hex_s.c_str(), 63);
            char* tok = strtok(buf, " ");
            while (tok && cnt < 8) { bytes[cnt++] = strtoul(tok, nullptr, 16); tok = strtok(nullptr, " "); }
            char label[LOG_INFO_LEN]; snprintf(label, LOG_INFO_LEN, "→ Manual 0x%03X", tx_id);
            if (can_lock()) {
                can_tx(tx_id, bytes, cnt, label);
                uint8_t frames[8][8]; uint8_t lens[8]; uint32_t ids[8];
                can_rx_collect(rx_filt, UDS_TIMEOUT_MS, frames, lens, ids, 8);
                can_unlock();
            }
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/scan", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            if (scan_running) { r->send(200, "application/json", "{\"err\":\"laeuft\"}"); return; }
            JsonDocument doc; deserializeJson(doc, data, len);
            int      mode    = doc["mode"]   | 1;
            uint32_t req_id  = strtoul((doc["reqId"]  | "7E6"), nullptr, 16);
            uint32_t resp_id = strtoul((doc["respId"] | "7EE"), nullptr, 16);
            const char* name = doc["name"]   | "Modul";
            can_start_scan(mode, req_id, resp_id, name);
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/scan-abort", HTTP_POST, [](AsyncWebServerRequest* r) {
        scan_abort = true;
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/monitor", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc; deserializeJson(doc, data, len);
            monitor_mode = doc["enable"] | false;
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/restart-can", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc; deserializeJson(doc, data, len);
            uint32_t br = doc["bitrate"] | CAN_SPEED_KBPS;
            can_stop();
            vTaskDelay(pdMS_TO_TICKS(300));
            bool ok = can_init(br);
            r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        }
    );

    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* r) {
        r->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        esp_restart();
    });

    server.on("/clear-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        log_clear();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/download-log", HTTP_GET, [](AsyncWebServerRequest* r) {
        String txt = log_to_txt();
        auto* resp = r->beginResponse(200, "text/plain; charset=utf-8", txt);
        resp->addHeader("Content-Disposition", "attachment; filename=\"vw_can_log.txt\"");
        r->send(resp);
    });

    // Status — inkl. RTC + Gyro + PMU Akku + GPS
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["fw_version"]  = FW_VERSION;
        doc["uptime"]      = millis();
        doc["can_ok"]      = can_hw_ok();
        doc["log_cnt"]     = log_count;
        doc["scan"]        = scan_running;
        doc["rtc_time"]    = rtc_now_str();
        doc["rtc_ok"]      = rtc_is_running();
        { int y,mo,d,h,mi,s;
          if (rtc_get_datetime(y,mo,d,h,mi,s) && y >= 2024) {
            char dbuf[12]; snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", y, mo, d);
            doc["rtc_date"] = dbuf;
          }
        }
        doc["gyro_state"]     = (int)gyro_get_state();  // 0=err 1=still 2=shake
        doc["gyro_ok"]        = gyro_ok();
        doc["gyro_g"]         = gyro_get_accel_g();
        doc["gyro_threshold"] = gyro_get_threshold();
        doc["gyro_mot_thr"]   = gyro_get_mot_threshold();
        doc["batt_pct"]    = pmu_batt_pct();          // -1=kein Akku, 0-100=%
        doc["vbus_in"]     = pmu_is_vbus_in();        // externe Spannung?
        doc["charging"]    = pmu_is_charging();        // Akku wird geladen?
        doc["gps_valid"]   = gps_valid();
        doc["gps_lat"]     = serialized(String(gps_lat(), 6));
        doc["gps_lon"]     = serialized(String(gps_lon(), 6));
        doc["gps_loc"]     = gps_location_str();
        doc["gps_sat"]     = modem_gps_usat();
        doc["gps_vsat"]    = modem_gps_vsat();
        doc["modem_ok"]    = modem_is_connected();
        doc["modem_sig"]   = (int)modem_signal_quality();
        doc["modem_op"]    = modem_operator();
        doc["modem_sim"]   = modem_sim_ok();
        String out; serializeJson(doc, out);
        auto* resp = r->beginResponse(200, "application/json", out);
        headers_apply(resp);
        r->send(resp);
    });

    // Gyro Schwelle setzen
    server.on("/gyro/set-threshold", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            float thr = doc["threshold"] | -1.0f;
            if (thr >= 0.005f && thr <= 1.0f) {
                gyro_set_threshold(thr);
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"ok\":false,\"err\":\"0.005–1.0\"}");
            }
        }
    );

    // Gyro Hardware-Aufwachschwelle setzen (MOT_THR, 1 Einheit = 32mg)
    server.on("/gyro/set-mot-threshold", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            int v = doc["value"] | -1;
            if (v >= 1 && v <= 255) {
                gyro_set_mot_threshold((uint8_t)v);
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"ok\":false,\"err\":\"1-255\"}");
            }
        }
    );

    // Gyro Baseline-Kalibrierung (Board muss ruhig stehen, ~3s Messung)
    server.on("/api/gyro/calibrate", HTTP_POST, [](AsyncWebServerRequest* r) {
        float baseline = 1.0f, stddev = 0.0f;
        bool ok = gyro_recalibrate(&baseline, &stddev);
        JsonDocument doc;
        doc["ok"]       = ok;
        doc["baseline"] = serialized(String(baseline, 4));
        doc["stddev"]   = serialized(String(stddev, 4));
        String out; serializeJson(doc, out);
        r->send(200, "application/json", out);
    });

    // Kompass-Kalibrierung zurücksetzen

    // GPS — Standort als JSON
    server.on("/gps", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["valid"] = gps_valid();
        doc["lat"]   = serialized(String(gps_lat(), 6));
        doc["lon"]   = serialized(String(gps_lon(), 6));
        doc["loc"]   = gps_location_str();
        String out; serializeJson(doc, out);
        auto* resp = r->beginResponse(200, "application/json", out);
        headers_apply(resp);
        r->send(resp);
    });

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);
    server.begin();
}

bool web_ap_active() { return s_ap_active; }

void web_ap_stop() {
    if (!s_ap_active) return;
    s_ap_active = false;
    s_had_client = false;
    ws.closeAll();
    server.end();
    WiFi.softAPdisconnect(true);
    syslog("WEB", "AP abgeschaltet");
}

void web_ap_start() {
    if (s_ap_active) return;
    WiFi.softAP(cfg_ap_ssid(), cfg_ap_pass());
    server.begin();
    s_ap_active   = true;
    s_had_client  = false;
    s_ap_start_ms = millis();
    syslog("WEB", "AP eingeschaltet (VBUS)");
}

// Im loop() aufrufen — schaltet AP nach 2 min ohne Client ab,
// startet ihn bei VBUS-Flanke (aus→an) wieder.
void web_ap_update() {
    static bool s_vbus_prev = true;   // Boot: VBUS wahrscheinlich an
    static uint8_t s_vbus_off_cnt = 0; // Entprellung: zählt aufeinanderfolgende false-Reads
    bool vbus = pmu_is_vbus_in();

    // VBUS-Entprellung: erst nach 3× false als "weg" werten (AXP2101 I2C-Glitch)
    if (!vbus) {
        if (s_vbus_off_cnt < 3) { s_vbus_off_cnt++; vbus = true; }
    } else {
        s_vbus_off_cnt = 0;
    }

    // VBUS Flanke aus→an → AP wieder einschalten
    if (vbus && !s_vbus_prev && !s_ap_active) {
        web_ap_start();
    }
    s_vbus_prev = vbus;

    if (!s_ap_active) return;

    // Client da? → Timer stoppen, merken
    bool has_client = (WiFi.softAPgetStationNum() > 0);
    if (has_client) {
        s_had_client = true;
        return;
    }

    // Hatte Client, ist jetzt weg → AP bleibt an (Seitenwechsel, kurze Pause)
    if (s_had_client) return;

    // Solange VBUS da ist: AP an lassen. Das Abschalten via softAPdisconnect()
    // ist ein IPC-Call auf Core 0 (ipc0-Task) und kann unter TELEM-Last
    // mit anderen ipc0-Operationen kollidieren — bisheriger PANIC-Trigger
    // waehrend Fahrt nach 2 min Boot ohne Client-Verbindung.
    if (vbus) return;

    // Kein VBUS, kein Client → 2-min-Timeout prüfen
    if ((millis() - s_ap_start_ms) >= AP_TIMEOUT_MS) {
        web_ap_stop();
    }
}

void ble_web_routes_init() {
    // HTMLs aus eingebettetem Flash — kein SPIFFS-Zugriff, kein Race, immer da.
    server.on("/debug",  HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, debug_html_start,  debug_html_end,  "text/html", true);
    });
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, config_html_start, config_html_end, "text/html", true);
    });
    server.on("/daten",  HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, daten_html_start,  daten_html_end,  "text/html", true);
    });

    // Shared Assets — ebenfalls aus dem Binary
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, style_css_start, style_css_end, "text/css");
    });
    server.on("/common.js", HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, common_js_start, common_js_end, "application/javascript");
    });
    server.on("/i18n.js", HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, i18n_js_start, i18n_js_end, "application/javascript");
    });
    server.on("/lang/de.json", HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, de_json_start, de_json_end, "application/json");
    });
    server.on("/lang/en.json", HTTP_GET, [](AsyncWebServerRequest* r) {
        send_embedded(r, en_json_start, en_json_end, "application/json");
    });

    // Silence browser favicon requests instead of 500 from missing handler.
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(204);
    });

    // /api/telemetry — alle Telemetrie-Felder mit Wert + Alter
    server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["ts_ms"]        = (uint32_t)millis();
        doc["mqtt_ok"]       = mqtt_ok();
        doc["mqtt_count"]    = mqtt_pub_count();
        doc["lte_connected"] = modem_is_connected();
        doc["buf_pending"]   = telem_get_buf_pending();
        doc["row_pending"]   = telem_get_row_pending();

        JsonObject fields = doc["fields"].to<JsonObject>();

        // Hilfslambda: float-Feld eintragen
        auto addField = [&](const char* name, TelemField f) {
            float v = 0.0f; uint32_t age = 0;
            bool valid = telem_get_latest(f, &v, &age);
            JsonObject o = fields[name].to<JsonObject>();
            o["v"]      = serialized(String(v, 6));
            o["age_ms"] = age;
            o["valid"]  = valid;
        };

        addField("soc",           TELEM_SOC);
        addField("voltage",       TELEM_VOLTAGE);
        addField("current",       TELEM_CURRENT);
        addField("power",         TELEM_POWER);
        addField("vehicle_speed", TELEM_VEHICLE_SPEED);
        addField("is_charging",   TELEM_IS_CHARGING);
        addField("is_dcfc",       TELEM_IS_DCFC);
        addField("batt_temp",     TELEM_BATT_TEMP);
        addField("ext_temp",      TELEM_EXT_TEMP);
        addField("gps_lat",       TELEM_GPS_LAT);
        addField("gps_lon",       TELEM_GPS_LON);
        addField("gps_valid",     TELEM_GPS_VALID);
        addField("gyro_g",        TELEM_GYRO_G);
        addField("lte_signal",    TELEM_LTE_SIGNAL);
        addField("capacity",      TELEM_CAPACITY);
        addField("kwh_charged",   TELEM_KWH_CHARGED);
        addField("is_parked",     TELEM_IS_PARKED);
        addField("odometer",      TELEM_ODOMETER);
        addField("range",         TELEM_RANGE);
        addField("batt_device",   TELEM_BATT_DEVICE);

        // lte_operator: string-Feld
        {
            float v = 0.0f; uint32_t age = 0;
            bool valid = telem_get_latest(TELEM_LTE_OPERATOR, &v, &age);
            JsonObject o = fields["lte_operator"].to<JsonObject>();
            o["v"]      = 0;
            o["age_ms"] = age;
            o["valid"]  = valid;
            o["str"]    = telem_get_operator_str();
        }

        String out; serializeJson(doc, out);
        auto* resp = r->beginResponse(200, "application/json", out);
        headers_apply(resp);
        r->send(resp);
    });

    // ISO-TP CAN PID Abfrage — POST {txid, rxid, data} → {ok, data: "hex bytes"}
    server.on("/api/canpid", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* body, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, body, len)) {
                r->send(400, "application/json", "{\"ok\":false}"); return;
            }
            uint32_t tx_id = strtoul(doc["txid"] | "0", nullptr, 16);
            uint32_t rx_id = strtoul(doc["rxid"] | "0", nullptr, 16);
            const char* data_str = doc["data"] | "";

            uint8_t req[8] = {}; uint8_t req_len = 0;
            char tmp[64]; strncpy(tmp, data_str, 63); tmp[63] = 0;
            char* tok = strtok(tmp, " ");
            while (tok && req_len < 8) {
                req[req_len++] = (uint8_t)strtoul(tok, nullptr, 16);
                tok = strtok(nullptr, " ");
            }

            if (!tx_id || !req_len) {
                r->send(400, "application/json", "{\"ok\":false}"); return;
            }

            uint8_t resp[64];
            int n = can_isotp_query(tx_id, rx_id, req, req_len, resp, sizeof(resp), 400);
            if (n <= 0) {
                r->send(200, "application/json", "{\"ok\":false}"); return;
            }

            // Hex-String aufbauen
            char hex[200] = "";
            for (int i = 0; i < n; i++) {
                char h[4];
                snprintf(h, sizeof(h), i == 0 ? "%02x" : " %02x", resp[i]);
                strncat(hex, h, sizeof(hex) - strlen(hex) - 1);
            }

            JsonDocument out_doc;
            out_doc["ok"]   = true;
            out_doc["data"] = hex;
            String out; serializeJson(out_doc, out);
            r->send(200, "application/json", out);
        }
    );


    server.on("/wifi/set-time", HTTP_POST,
        [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400); return; }
            int h = doc["hour"] | -1, m = doc["minute"] | 0, s = doc["second"] | 0;
            int yr = doc["year"] | 0, mo = doc["month"] | 0, dy = doc["day"] | 0;
            if (h >= 0 && h < 24 && m >= 0 && m < 60) {
                if (yr >= 2024 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
                    rtc_set_datetime(yr, mo, dy, h, m, s);
                    char msg[48];
                    snprintf(msg, sizeof(msg), "Datum+Zeit gesetzt: %04d-%02d-%02d %02d:%02d:%02d", yr, mo, dy, h, m, s);
                    syslog("RTC", msg);
                } else {
                    rtc_set_time(h, m, s);
                }
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"ok\":false}");
            }
        }
    );

    server.on("/wifi/scan-start", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (WiFi.softAPgetStationNum() > 0) {
            r->send(200, "application/json", "{\"ok\":false,\"err\":\"client_connected\"}"); return;
        }
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/wifi/scan-result", HTTP_GET, [](AsyncWebServerRequest* r) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) { r->send(200, "application/json", "{\"status\":\"running\"}"); return; }
        if (n < 0)                  { r->send(200, "application/json", "{\"status\":\"failed\"}");  return; }
        JsonDocument doc;
        doc["status"] = "done";
        doc["count"]  = n;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            int rssi = WiFi.RSSI(i);
            if (rssi < -70) continue;
            JsonObject net = arr.add<JsonObject>();
            net["ssid"]     = WiFi.SSID(i);
            net["rssi"]     = rssi;
            net["channel"]  = WiFi.channel(i);
            net["dist"]     = rssi > -55 ? "<5m" : rssi > -65 ? "~10m" : rssi > -75 ? "~20m" : ">30m";
        }
        String out; serializeJson(doc, out);
        r->send(200, "application/json", out);
    });

    // Logs koennen gross werden (bis 1MB) — nicht in Heap laden.
    // Snapshot via Rename: andere Tasks schreiben dann auf neue Datei,
    // Async-Read der Snapshot-Datei kollidiert nicht mehr mit Writern.
    auto snapshot_and_send = [](AsyncWebServerRequest* r, const char* path, const char* snap) {
        if (!spiffs_lock(1000)) { r->send(503, "text/plain", "busy"); return; }
        if (SPIFFS.exists(snap)) SPIFFS.remove(snap);
        bool have = SPIFFS.exists(path) && SPIFFS.rename(path, snap);
        spiffs_unlock();
        if (!have) { r->send(200, "text/plain", "# leer\n"); return; }
        r->send(SPIFFS, snap, "text/plain");
    };

    server.on("/download-sys-log", HTTP_GET, [snapshot_and_send](AsyncWebServerRequest* r) {
        snapshot_and_send(r, "/sys.log", "/sys.log.snap");
    });
    server.on("/clear-sys-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (spiffs_lock(500)) {
            if (SPIFFS.exists("/sys.log"))      SPIFFS.remove("/sys.log");
            if (SPIFFS.exists("/sys.log.snap")) SPIFFS.remove("/sys.log.snap");
            spiffs_unlock();
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/download-elm-log", HTTP_GET, [snapshot_and_send](AsyncWebServerRequest* r) {
        snapshot_and_send(r, "/elm.log", "/elm.log.snap");
    });
    server.on("/clear-elm-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (spiffs_lock(500)) {
            if (SPIFFS.exists("/elm.log"))      SPIFFS.remove("/elm.log");
            if (SPIFFS.exists("/elm.log.snap")) SPIFFS.remove("/elm.log.snap");
            spiffs_unlock();
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // ── Telemetrie-Puffer Preview (Debug) ───────────────────────
    server.on("/api/row-preview", HTTP_GET, [](AsyncWebServerRequest* r) {
        auto* resp = r->beginResponse(200, "text/plain", telem_preview_rows(10));
        headers_apply(resp);
        r->send(resp);
    });

    // ── Laufzeit-Config (NVS) ──────────────────────────────────
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
        auto* resp = r->beginResponse(200, "application/json", cfg_to_json());
        headers_apply(resp);
        r->send(resp);
    });

    server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            bool ok = cfg_save_json(data, len);
            r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        }
    );

    // ── WiFi-Upload Test ───────────────────────────────────────
    // POST {"slot":0|1} → kicks off a connect+dummy-publish test for the
    // selected slot. Result lands in syslog (Debug page). Response is
    // just an ack — the actual work is async on the device.
    server.on("/api/wifi-test", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                r->send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
                return;
            }
            int slot = doc["slot"] | -1;
            if (slot < 0 || slot >= WIFI_UPLOAD_SLOTS) {
                r->send(400, "application/json", "{\"ok\":false,\"err\":\"slot out of range\"}");
                return;
            }
            bool ok = wifi_upload_test_slot(slot);
            r->send(200, "application/json",
                    ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"slot empty or busy\"}");
        }
    );

    // ── OTA Firmware-Upload ────────────────────────────────────
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* r) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp = r->beginResponse(
                200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
            resp->addHeader("Connection", "close");
            r->send(resp);
            if (ok) {
                syslog("OTA", "Firmware-Update OK \xe2\x80\x93 Neustart\xe2\x80\xa6");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        },
        [](AsyncWebServerRequest* r, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                    Serial.println("[OTA] begin() Fehler");
            }
            if (Update.write(data, len) != len)
                Serial.println("[OTA] write() Fehler");
            if (final) {
                if (Update.end(true))
                    Serial.printf("[OTA] OK: %u Bytes\n", index + len);
                else
                    Serial.println("[OTA] end() Fehler");
            }
        }
    );
}