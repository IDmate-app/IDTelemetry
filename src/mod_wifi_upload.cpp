#include "mod_wifi_upload.h"
#include "mod_logs.h"
#include "mod_telem_store.h"
#include "mod_telemetry.h"
#include "mod_payload.h"
#include "mod_sleep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <espMqttClient.h>   // FIXES B.4: QoS-1-Publish mit PUBACK statt PubSubClient (QoS 0)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>

#if FEATURE_WIFI_UPLOAD

// ============================================================
//  Two-slot WiFi STA + drain telem_store via HTTP POST or MQTT.
//
//  URL scheme dispatch (per slot):
//    http://host[:port]/path   → HTTP POST, body = AES-encrypted bytes
//    https://host[:port]/path  → HTTPS POST (TLS via WiFiClientSecure)
//    mqtt://host[:port]/topic  → MQTT publish, payload = encrypted bytes
//    mqtts://host[:port]/topic → MQTT over TLS (TODO)
//
//  Server payload is BYTE-IDENTICAL to the LTE/MQTT path — same
//  AES-256-CBC binary format produced by mod_payload. IDMate decoder
//  works unchanged for all transports.
//
//  AP guard:
//    When the local AP has any associated station (user configuring
//    via /config), we skip the scan/connect cycle. AP_STA channel
//    hopping would otherwise drop the phone mid-edit.
//
//  Note: AsyncTCP also runs in WIFI_MODE_STA for the web server. We
//  keep WIFI_AP_STA explicitly so the AP remains usable when STA is up.
// ============================================================

struct Slot {
    char ssid[64];
    char pass[64];
    char url[128];
};
static Slot     s_slots[WIFI_UPLOAD_SLOTS] = {};
static int      s_active_slot              = -1;
static uint32_t s_uploaded                 = 0;
static bool     s_task_running             = false;
static uint32_t s_last_scan_ms             = 0;
static const uint32_t SCAN_INTERVAL_MS     = 30000UL;
static const uint32_t CONNECT_TIMEOUT_MS   = 12000UL;
static const uint32_t DRAIN_BATCH          = 20;     // rows per cycle
static const uint32_t DRAIN_GAP_MS         = 50;     // pause between rows

// MQTT-over-WiFi client (lazy-connect when first mqtt:// slot publishes).
// UseInternalTask::NO → kein eigener Lib-Task; wir pumpen loop() selbst im
// Drain-Task und können so synchron auf das PUBACK warten (FIXES B.4).
static espMqttClient s_mqtt(espMqttClientTypes::UseInternalTask::NO);
static volatile uint16_t s_mqtt_acked_pid = 0;   // packetId des letzten PUBACK
static void on_mqtt_publish(uint16_t packetId) { s_mqtt_acked_pid = packetId; }

void wifi_upload_configure_slot(int idx, const char* ssid, const char* pass,
                                const char* post_url) {
    if (idx < 0 || idx >= WIFI_UPLOAD_SLOTS) return;
    Slot& s = s_slots[idx];
    if (ssid)     { strncpy(s.ssid, ssid, sizeof(s.ssid) - 1); s.ssid[sizeof(s.ssid) - 1] = '\0'; }
    else          { s.ssid[0] = '\0'; }
    if (pass)     { strncpy(s.pass, pass, sizeof(s.pass) - 1); s.pass[sizeof(s.pass) - 1] = '\0'; }
    else          { s.pass[0] = '\0'; }
    if (post_url) { strncpy(s.url, post_url, sizeof(s.url) - 1); s.url[sizeof(s.url) - 1] = '\0'; }
    else          { s.url[0] = '\0'; }

    char m[96];
    snprintf(m, sizeof(m), "Slot %d: ssid=%s url=%s",
             idx, s.ssid[0] ? s.ssid : "(none)", s.url[0] ? s.url : "(none)");
    syslog("WIFI_UP", m);
}

bool wifi_upload_is_connected() {
    return (WiFi.getMode() & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED;
}

int      wifi_upload_active_slot() { return s_active_slot; }
uint32_t wifi_upload_count()       { return s_uploaded; }

// Test trigger — picked up by the main task on its next tick.
static volatile int s_test_request = -1;
bool wifi_upload_test_slot(int slot) {
    if (slot < 0 || slot >= WIFI_UPLOAD_SLOTS) return false;
    if (!s_slots[slot].ssid[0]) return false;
    if (s_test_request >= 0) return false;  // already running
    s_test_request = slot;
    char m[64]; snprintf(m, sizeof(m), "Test angefordert für Slot %d", slot);
    syslog("WIFI_UP", m);
    return true;
}

static bool any_slot_enabled() {
    for (int s = 0; s < WIFI_UPLOAD_SLOTS; s++) {
        if (s_slots[s].ssid[0]) return true;
    }
    return false;
}

// ── Scan + connect ─────────────────────────────────────────
static int find_best_slot_in_range() {
    int n = WiFi.scanNetworks();   // synchronous, ~2-3 s
    if (n <= 0) return -1;
    int best = -1;
    int best_rssi = -127;
    for (int i = 0; i < n; i++) {
        String found = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        for (int s = 0; s < WIFI_UPLOAD_SLOTS; s++) {
            if (!s_slots[s].ssid[0]) continue;
            if (found != s_slots[s].ssid) continue;
            if (best < 0 || s < best || (s == best && rssi > best_rssi)) {
                best = s; best_rssi = rssi;
            }
        }
    }
    WiFi.scanDelete();
    return best;
}

static bool try_connect(int slot) {
    if (slot < 0 || slot >= WIFI_UPLOAD_SLOTS) return false;
    if (!s_slots[slot].ssid[0]) return false;
    char m[80];
    snprintf(m, sizeof(m), "Slot %d verbinden: %s", slot, s_slots[slot].ssid);
    syslog("WIFI_UP", m);

    WiFi.begin(s_slots[slot].ssid, s_slots[slot].pass);
    uint32_t start = millis();
    while (millis() - start < CONNECT_TIMEOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) {
            s_active_slot = slot;
            snprintf(m, sizeof(m), "Slot %d STA verbunden · IP %s",
                     slot, WiFi.localIP().toString().c_str());
            syslog("WIFI_UP", m);
            return true;
        }
        if (g_shutdown) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    snprintf(m, sizeof(m), "Slot %d Timeout", slot);
    syslog("WIFI_UP", m);
    WiFi.disconnect();
    return false;
}

// ── URL parsing ────────────────────────────────────────────
// Returns: 0=http, 1=https, 2=mqtt, 3=mqtts, -1=unknown
static int parse_scheme(const char* url) {
    if (strncmp(url, "http://", 7)   == 0) return 0;
    if (strncmp(url, "https://", 8)  == 0) return 1;
    if (strncmp(url, "mqtt://", 7)   == 0) return 2;
    if (strncmp(url, "mqtts://", 8)  == 0) return 3;
    return -1;
}

// Split mqtt://host:port/topic into host, port, topic.
// Caller provides buffers. Returns true on success.
static bool parse_mqtt_url(const char* url, char* host, size_t host_size,
                           uint16_t* port, char* topic, size_t topic_size) {
    int scheme = parse_scheme(url);
    if (scheme != 2 && scheme != 3) return false;
    const char* p = url + (scheme == 3 ? 8 : 7);   // skip scheme

    // host[:port]
    const char* slash = strchr(p, '/');
    if (!slash) return false;
    size_t hostpart_len = slash - p;
    char hostpart[96];
    if (hostpart_len >= sizeof(hostpart)) return false;
    memcpy(hostpart, p, hostpart_len);
    hostpart[hostpart_len] = '\0';

    char* colon = strchr(hostpart, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        *port = (scheme == 3) ? 8883 : 1883;
    }
    strncpy(host, hostpart, host_size - 1);
    host[host_size - 1] = '\0';

    strncpy(topic, slash + 1, topic_size - 1);
    topic[topic_size - 1] = '\0';
    return host[0] && topic[0];
}

// ── HTTP POST a single encoded row ────────────────────────
static bool http_post_payload(const char* url, const uint8_t* body, int body_len) {
    HTTPClient http;
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/octet-stream");
    http.setTimeout(8000);
    int code = http.POST((uint8_t*)body, body_len);
    http.end();
    return code >= 200 && code < 300;
}

// ── MQTT publish a single encoded row ─────────────────────
static bool mqtt_publish_payload(const char* host, uint16_t port, const char* topic,
                                 const uint8_t* body, int body_len) {
    if (!s_mqtt.connected()) {
        char client_id[24];
        snprintf(client_id, sizeof(client_id), "idtelem-%08x",
                 (unsigned)((uint32_t)ESP.getEfuseMac() & 0xFFFFFFFFu));
        s_mqtt.setClientId(client_id);
        s_mqtt.setServer(host, port);
        s_mqtt.setKeepAlive(30);
        s_mqtt.connect();   // async — wir pumpen loop() bis connected() / Timeout
        uint32_t t0 = millis();
        while (!s_mqtt.connected() && millis() - t0 < 8000) {
            s_mqtt.loop();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s_mqtt.connected()) {
            syslog("WIFI_UP", "MQTT-WiFi connect timeout");
            return false;
        }
        char m[80]; snprintf(m, sizeof(m), "MQTT-WiFi connected (QoS1): %s:%u", host, port);
        syslog("WIFI_UP", m);
    }

    // FIXES B.4: QoS-1-Publish und auf PUBACK warten. Erst wenn der Broker den
    // Empfang bestätigt hat, gibt true zurück → der Caller ackt die Row aus dem
    // Store. Bricht WiFi/Verbindung vorher ab, bleibt die Row erhalten.
    s_mqtt_acked_pid = 0;
    uint16_t pid = s_mqtt.publish(topic, /*qos*/1, /*retain*/false, body, (size_t)body_len);
    if (pid == 0) {
        syslog("WIFI_UP", "MQTT-WiFi publish nicht eingereiht (Puffer voll/getrennt)");
        return false;
    }
    uint32_t t0 = millis();
    while (s_mqtt_acked_pid != pid && millis() - t0 < 8000) {
        if (!s_mqtt.connected()) break;   // Verbindung weg → kein PUBACK mehr
        s_mqtt.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_mqtt_acked_pid == pid;
}

// ── One row through the active slot's URL ─────────────────
static bool send_one_row(const TelemetryRow& row, const char* url) {
    static uint8_t encbuf[128];
    int n = payload_encode(row, encbuf, sizeof(encbuf));
    if (n <= 0) return false;

    int scheme = parse_scheme(url);
    switch (scheme) {
        case 0:   // http
        case 1:   // https — Arduino HTTPClient handles both via begin(url)
            return http_post_payload(url, encbuf, n);

        case 2: { // mqtt
            char host[96]; uint16_t port = 1883; char topic[64];
            if (!parse_mqtt_url(url, host, sizeof(host), &port, topic, sizeof(topic))) return false;
            return mqtt_publish_payload(host, port, topic, encbuf, n);
        }

        case 3:   // mqtts — placeholder; needs WiFiClientSecure wiring
        default:
            return false;
    }
}

// ── AP guard ───────────────────────────────────────────────
// Skip the STA cycle while someone is on the AP. Avoids the
// AP_STA channel-hop that breaks active /config sessions.
static bool ap_is_busy() {
    return WiFi.softAPgetStationNum() > 0;
}

// Idle-Sleep, der bei Shutdown sofort aussteigt — so muss enter_deep_sleep()
// uns nicht mitten in einem langen vTaskDelay force-killen (FIXES B.3).
static void shutdown_aware_delay(uint32_t ms) {
    const uint32_t step = 200;
    while (ms > 0 && !g_shutdown) {
        uint32_t chunk = ms < step ? ms : step;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
}

// ── Main task ──────────────────────────────────────────────
static void wifi_upload_task(void*) {
    s_task_running = true;
    payload_init_key();
    s_mqtt.onPublish(on_mqtt_publish);   // FIXES B.4: PUBACK-Tracking (einmalig)

    while (!g_shutdown) {
        // Handle one-shot test request from the Web UI ──────
        if (s_test_request >= 0) {
            int slot = s_test_request;
            s_test_request = -1;
            char m[96];

            // Heads-up: the AP will wobble while STA joins (shared radio).
            // The phone that triggered the test usually drops + reconnects.
            syslog("WIFI_UP", "TEST start — AP wackelt kurz, Phone reconnectet danach");
            snprintf(m, sizeof(m), "TEST Slot %d → %s",
                     slot, s_slots[slot].url[0] ? s_slots[slot].url : "(no URL)");
            syslog("WIFI_UP", m);

            // If already connected to a different slot, drop it first.
            if (wifi_upload_is_connected() && s_active_slot != slot) {
                WiFi.disconnect(false);
                s_active_slot = -1;
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            bool connected = wifi_upload_is_connected() || try_connect(slot);
            bool payload_ok = false;
            if (connected) {
                // Synthesise a dummy row so we send SOMETHING even with empty store.
                TelemetryRow t = {};
                t.unix_s = (uint32_t)time(NULL);
                const char* url = s_slots[slot].url;
                if (url[0]) payload_ok = send_one_row(t, url);
            }

            if (!connected)        syslog("WIFI_UP", "TEST: ✗ Slot konnte nicht verbinden");
            else if (!payload_ok)  syslog("WIFI_UP", "TEST: ✗ verbunden aber Endpoint lehnt ab / URL fehlt");
            else                   syslog("WIFI_UP", "TEST: ✓ Verbindung + Endpoint OK");

            // Release STA immediately so the AP returns to its own channel
            // and the user's phone can re-associate quickly.
            if (s_mqtt.connected()) s_mqtt.disconnect(true);
            if (wifi_upload_is_connected()) WiFi.disconnect(false);
            s_active_slot = -1;
            syslog("WIFI_UP", "TEST done — STA freigegeben, AP wieder stabil");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Nothing configured? Idle.
        if (!any_slot_enabled()) {
            shutdown_aware_delay(15000);
            continue;
        }

        // Block STA while AP has clients (race avoidance).
        if (ap_is_busy()) {
            if (wifi_upload_is_connected()) {
                syslog("WIFI_UP", "AP-Client aktiv → STA pausiert");
                WiFi.disconnect(false);
                s_active_slot = -1;
            }
            shutdown_aware_delay(5000);
            continue;
        }

        // Reconnect cycle when not associated.
        if (!wifi_upload_is_connected()) {
            if (s_active_slot >= 0) {
                char m[48]; snprintf(m, sizeof(m), "Slot %d getrennt", s_active_slot);
                syslog("WIFI_UP", m);
                s_active_slot = -1;
                if (s_mqtt.connected()) s_mqtt.disconnect(true);
            }
            uint32_t now = millis();
            if (now - s_last_scan_ms >= SCAN_INTERVAL_MS) {
                s_last_scan_ms = now;
                int slot = find_best_slot_in_range();
                if (slot >= 0) try_connect(slot);
            }
            shutdown_aware_delay(5000);
            continue;
        }

        // Connected — drain telem_store via the active slot's URL.
        const char* url = s_slots[s_active_slot].url;
        if (!url[0] || parse_scheme(url) < 0) {
            // No URL configured — nothing to send, just keep STA alive.
            shutdown_aware_delay(10000);
            continue;
        }

        uint32_t sent_this_cycle = 0;
        TelemetryRow row;
        while (telem_store_pending() > 0 && sent_this_cycle < DRAIN_BATCH) {
            if (!telem_store_peek(row)) break;
            if (!send_one_row(row, url)) break;        // backoff on error
            telem_store_ack();
            s_uploaded++;
            sent_this_cycle++;
            if (s_mqtt.connected()) s_mqtt.loop();     // service MQTT
            vTaskDelay(pdMS_TO_TICKS(DRAIN_GAP_MS));
            if (g_shutdown) break;
        }

        if (sent_this_cycle > 0) {
            char m[80];
            snprintf(m, sizeof(m), "Slot %d: %u Rows hochgeladen (total %u)",
                     s_active_slot, (unsigned)sent_this_cycle, (unsigned)s_uploaded);
            syslog("WIFI_UP", m);
        }

        if (s_mqtt.connected()) s_mqtt.loop();
        shutdown_aware_delay(3000);
    }

    if (s_mqtt.connected()) s_mqtt.disconnect(true);   // force: kein loop()-Pumpen mehr im Shutdown
    if (wifi_upload_is_connected()) WiFi.disconnect(true);
    s_task_running = false;
    vTaskDelete(nullptr);
}

void wifi_upload_start_task() {
    if (s_task_running) return;
    xTaskCreatePinnedToCore(wifi_upload_task, "WIFI_UP", 6144, nullptr, 1, nullptr, 0);
    syslog("WIFI_UP", "Task gestartet (Drain aktiv, AP-Guard ein)");
}

#endif  // FEATURE_WIFI_UPLOAD
