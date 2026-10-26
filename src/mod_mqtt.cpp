// TINY_GSM_MODEM_SIM7080 muss VOR dem ersten TinyGSM-Include definiert sein.
#ifndef TINY_GSM_MODEM_SIM7080
#define TINY_GSM_MODEM_SIM7080
#endif

#include <TinyGsmClient.h>

#include "mod_mqtt.h"
#include "mod_config.h"
#include "mod_pmu.h"
#include "mod_logs.h"
#include "mod_rtc.h"
#include "mod_payload.h"
#include "shared.h"
#include "config.h"
#include "secrets.h"
#include <Arduino.h>
#include <esp_system.h>
// AES + RNG includes belong to mod_payload.cpp now.

// ---- Extern: TinyGsm + Serial aus mod_modem ----
extern TinyGsm&        modem_get();
extern HardwareSerial&  modem_serial();

// AES + binary build + BF_* enum live in mod_payload.h/.cpp now
// (shared with mod_wifi_upload). See payload_encode().

// ---- Zustand ----
static bool     s_configured   = false;
static bool     s_connected    = false;
static bool     s_last_pub_ok  = false;
static uint32_t s_last_pub_ms  = 0;
static uint16_t s_pub_count    = 0;
static char     s_last_ack[128] = "";
static char     s_sub_topic[80] = "";   // Subscribe-Topic für Server-ACK

// ---- AT-Hilfe (wie in mod_modem) ----
static bool mqtt_at_ok(const char* cmd, long timeout_ms = 5000L) {
    TinyGsm& m = modem_get();
    m.sendAT(cmd);
    String resp = "";
    int rc = m.waitResponse(timeout_ms, resp);
    if (rc != 1) {
        char msg[128];
        resp.trim();
        snprintf(msg, sizeof(msg), "AT%s → ERROR (%s)", cmd, resp.c_str());
        syslog("MQTT", msg);
    }
    return rc == 1;
}

// ---- Konfiguration ----
void mqtt_configure() {
    payload_init_key();

    TinyGsm& m = modem_get();
    char cmd[160];

    // Alte Verbindung trennen (falls noch offen)
    m.sendAT("+SMDISC");
    m.waitResponse(3000L);

    // URL inkl. Port (SIM7080G: AT+SMCONF="URL","host","port")
    snprintf(cmd, sizeof(cmd), "+SMCONF=\"URL\",\"%s\",\"%d\"",
             cfg_mqtt_host(), cfg_mqtt_port());
    mqtt_at_ok(cmd);

    // Client-ID aus Topic-Prefix (z.B. "tele/vw_nox" → "vw_nox")
    {
        const char* topic = cfg_mqtt_topic();
        const char* slash = strrchr(topic, '/');
        const char* cid = slash ? slash + 1 : topic;
        snprintf(cmd, sizeof(cmd), "+SMCONF=\"CLIENTID\",\"%s\"", cid);
        mqtt_at_ok(cmd);
    }

    // Kein Username/Password — Auth über AES-Key im Payload

    snprintf(cmd, sizeof(cmd), "+SMCONF=\"KEEPTIME\",%d", MQTT_KEEPALIVE_S);
    mqtt_at_ok(cmd);

    mqtt_at_ok("+SMCONF=\"CLEANSS\",1");   // Clean Session
    mqtt_at_ok("+SMCONF=\"QOS\",1");        // Default QoS 1

    // Plain MQTT — kein TLS (SIM7080G FW R1951.07 hat kein MQTT-TLS).
    // Auth über Username/Password. Broker muss plain Listener haben.

    // Subscribe-Topic vorbereiten
    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s/ack", cfg_mqtt_topic());

    s_configured = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Konfiguriert: %s:%d topic=%s",
             cfg_mqtt_host(), cfg_mqtt_port(), cfg_mqtt_topic());
    syslog("MQTT", msg);
}

// ---- Verbinden ----
// SMCONN mit eigenem harten Timeout — TinyGSM waitResponse kann hängen
// wenn das Modem kontinuierlich URCs sendet (innere Leseschleife blockiert
// die Timeout-Prüfung in der äußeren do-while-Schleife).
static bool mqtt_smconn(uint32_t hard_timeout_ms = 20000UL) {
    HardwareSerial& ser = modem_serial();

    // Serial-Puffer leeren (alte URCs entsorgen)
    while (ser.available()) ser.read();

    ser.print("AT+SMCONN\r\n");

    uint32_t start = millis();
    String   buf;
    buf.reserve(128);

    while (millis() - start < hard_timeout_ms) {
        while (ser.available()) {
            char c = (char)ser.read();
            if (c == '\n') {
                buf.trim();
                if (buf == "OK")    return true;
                if (buf == "ERROR") return false;
                buf = "";
            } else {
                buf += c;
                // Sicherheit: Puffer begrenzen (URCs können lang sein)
                if (buf.length() > 200) buf = buf.substring(buf.length() - 80);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;  // Hard-Timeout
}

bool mqtt_connect() {
    if (!s_configured) mqtt_configure();

    syslog("MQTT", "Verbinde...");
    bool ok = mqtt_smconn(20000UL);
    if (!ok) {
        syslog("MQTT", "Verbindung fehlgeschlagen");
        s_connected = false;
        return false;
    }

    s_connected = true;
    syslog("MQTT", "Verbunden");

    // ACK-Topic abonnieren
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "+SMSUB=\"%s\",%d", s_sub_topic, MQTT_QOS);
    if (mqtt_at_ok(cmd, 5000L)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Subscribed: %s", s_sub_topic);
        syslog("MQTT", msg);
    }

    return true;
}

// ---- Verbindungsstatus ----
bool mqtt_is_connected() {
    if (!s_configured) return false;

    TinyGsm& m = modem_get();
    m.sendAT("+SMSTATE?");
    String resp = "";
    int rc = m.waitResponse(3000L, resp);
    if (rc != 1) {
        s_connected = false;
        return false;
    }
    // Antwort: +SMSTATE: 0 oder +SMSTATE: 1
    // FIXES B.17: gegen "+SMSTATE: 1" matchen, nicht gegen irgendeine '1' —
    // sonst matcht jede '1' in URCs/Echos → falsch-positives "connected".
    s_connected = (resp.indexOf("+SMSTATE: 1") >= 0);
    return s_connected;
}

// put_*, build_binary, encrypt_payload moved to mod_payload.cpp.

// ---- Publish einer Zeile ----
bool mqtt_publish_row(const TelemetryRow& row) {
    if (!s_connected) return false;

    TinyGsm& m = modem_get();
    HardwareSerial& ser = modem_serial();

    // Binär packen + AES verschlüsseln (shared mit WiFi-Upload via mod_payload)
    static uint8_t payload[192];   // 1 + 16 + max 128 padded
    int plen = payload_encode(row, payload, sizeof(payload));
    if (plen <= 0) return false;

    // Topic
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/data", cfg_mqtt_topic());

    // AT+SMPUB="topic",len,qos,retain
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "+SMPUB=\"%s\",%d,%d,0", topic, plen, MQTT_QOS);
    m.sendAT(cmd);

    // Warte auf ">" Prompt
    String resp = "";
    int rc = m.waitResponse(5000L, resp, ">", "ERROR");
    if (rc != 1) {
        syslog("MQTT", "SMPUB: kein > Prompt");
        s_connected = false;
        s_last_pub_ok = false;
        return false;
    }

    // Payload senden
    ser.write(payload, plen);

    // Warte auf OK (Broker PUBACK bei QoS 1)
    resp = "";
    rc = m.waitResponse(MQTT_PUBLISH_TIMEOUT_MS, resp);
    if (rc != 1) {
        resp.trim();
        char msg[128];
        snprintf(msg, sizeof(msg), "Publish fehlgeschlagen: %s", resp.c_str());
        syslog("MQTT", msg);
        s_connected = false;
        s_last_pub_ok = false;
        return false;
    }

    s_last_pub_ok = true;
    s_last_pub_ms = millis();
    s_pub_count++;
    Serial.printf("[MQTT] → %s (%d B enc) #%u\n", topic, plen, s_pub_count);
    return true;
}

// ---- Eingehende Nachrichten verarbeiten ----
void mqtt_poll() {
    // URCs wie +SMSUB: "topic",len,"payload" werden vom TinyGSM-Parser
    // in den Stream-Buffer geschrieben. Wir lesen verfügbare Bytes.
    HardwareSerial& ser = modem_serial();
    static char urc_buf[256];
    static int  urc_pos = 0;

    while (ser.available()) {
        char c = ser.read();
        if (c == '\n') {
            urc_buf[urc_pos] = '\0';
            // +SMSUB: "topic",len,"payload"
            if (strncmp(urc_buf, "+SMSUB:", 7) == 0) {
                // Payload extrahieren (nach letztem Anführungszeichen)
                char* last_q = strrchr(urc_buf, '"');
                char* first_payload_q = nullptr;
                if (last_q) {
                    // Rückwärts zum öffnenden " des Payloads
                    *last_q = '\0';
                    first_payload_q = strrchr(urc_buf, '"');
                    if (first_payload_q) {
                        first_payload_q++;  // nach dem "
                        strncpy(s_last_ack, first_payload_q, sizeof(s_last_ack) - 1);
                        s_last_ack[sizeof(s_last_ack) - 1] = '\0';
                        Serial.printf("[MQTT] ← ACK: %s\n", s_last_ack);
                        syslog("MQTT", s_last_ack);
                    }
                }
            }
            urc_pos = 0;
        } else if (urc_pos < (int)sizeof(urc_buf) - 1) {
            urc_buf[urc_pos++] = c;
        }
        // Nicht zu lange blockieren — max 10 Bytes pro Aufruf
        // (Rest wird beim nächsten poll() gelesen)
    }
}

// ---- Disconnect ----
void mqtt_disconnect() {
    if (!s_connected) return;
    mqtt_at_ok("+SMDISC", 5000L);
    s_connected = false;
    syslog("MQTT", "Getrennt");
}

// ---- Getter ----
bool        mqtt_ok()           { return s_last_pub_ok; }
uint32_t    mqtt_last_pub_ms()  { return s_last_pub_ms; }
uint16_t    mqtt_pub_count()    { return s_pub_count; }
const char* mqtt_last_ack()     { return s_last_ack; }

// ---- Debug-Info ----
void mqtt_print_info() {
    Serial.printf("[MQTT] Host: %s:%d\n", cfg_mqtt_host(), cfg_mqtt_port());
    Serial.printf("[MQTT] Topic: %s\n", cfg_mqtt_topic());
    Serial.printf("[MQTT] Connected: %s\n", s_connected ? "ja" : "nein");
    Serial.printf("[MQTT] Publishes: %u\n", s_pub_count);
    Serial.printf("[MQTT] Last OK: %s\n", s_last_pub_ok ? "ja" : "nein");
    if (s_last_ack[0])
        Serial.printf("[MQTT] Last ACK: %s\n", s_last_ack);
    if (s_last_pub_ms > 0)
        Serial.printf("[MQTT] Seit letztem Publish: %lus\n",
                       (unsigned long)(millis() - s_last_pub_ms) / 1000UL);
}
