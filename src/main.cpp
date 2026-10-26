/*
 * ============================================================
 *  Telemetry Stick - main.cpp
 *  Hardware: LILYGO T-SIM7080G-S3 (ESP32-S3-WROOM-1-N16R8) + SN65HVD230
 *
 *  Verdrahtung CAN:
 *   ESP32 GPIO17 → SN65HVD230 CTX
 *   ESP32 GPIO18 → SN65HVD230 CRX
 *   ESP32 3.3V   → SN65HVD230 VCC
 *   ESP32 GND    → SN65HVD230 GND
 *   SN65HVD230 CANH → OBD2 Pin 6
 *   SN65HVD230 CANL → OBD2 Pin 14
 *
 *  Verdrahtung I2C (DS1307 + MPU-6050):
 *   ESP32 GPIO45 → SDA
 *   ESP32 GPIO21 → SCL
 *   DS1307: I2C 0x68, CR2032 in Halter
 *   MPU-6050: AD0 an 3.3V → I2C 0x69
 *
 *  URL:   http://192.168.4.1
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

#include "config.h"
#include "shared.h"
#include "mod_can.h"
#include "mod_logs.h"
#include "mod_web.h"
#include "mod_elm327.h"
#include "mod_ble_scan.h"
#include "mod_rtc.h"
#include "mod_gyro.h"
#include "mod_gps_ext.h"
#include "mod_pmu.h"
#include "mod_modem.h"
#include "mod_mqtt.h"
#include "mod_sleep.h"
#include "mod_telemetry.h"
#include "mod_wifi_upload.h"
#include "mod_config.h"
#include <NimBLEDevice.h>

AsyncWebServer    server(80);
AsyncWebSocket    ws("/ws");
SemaphoreHandle_t logMutex = NULL;
GpsCache          g_gps;        // Zentraler GPS-Cache — alle Module lesen/schreiben hier

// ── GPS Spinlock — schützt g_gps gegen Cross-Core-Tearing ──
static portMUX_TYPE s_gps_mux = portMUX_INITIALIZER_UNLOCKED;

void gps_update(double lat, double lon, float speed_kmh, float course_deg) {
    // FIXES B.17: snprintf NICHT unter portENTER_CRITICAL (Cross-Core-Spinlock +
    // deaktivierte Interrupts → Latenz/Lock-Risiko). Vorher formatieren, unter
    // dem Lock nur noch memcpy.
    char loc[sizeof(g_gps.loc)];
    snprintf(loc, sizeof(loc), "%.6f %.6f", lat, lon);
    portENTER_CRITICAL(&s_gps_mux);
    g_gps.lat        = lat;
    g_gps.lon        = lon;
    g_gps.speed_kmh  = speed_kmh;
    g_gps.course_deg = course_deg;
    memcpy(g_gps.loc, loc, sizeof(g_gps.loc));
    g_gps.valid = true;
    portEXIT_CRITICAL(&s_gps_mux);
}

void gps_invalidate() {
    portENTER_CRITICAL(&s_gps_mux);
    g_gps.valid  = false;
    g_gps.loc[0] = '\0';
    portEXIT_CRITICAL(&s_gps_mux);
}

bool gps_valid() {
    portENTER_CRITICAL(&s_gps_mux);
    bool v = g_gps.valid;
    portEXIT_CRITICAL(&s_gps_mux);
    return v;
}

double gps_lat() {
    portENTER_CRITICAL(&s_gps_mux);
    double v = g_gps.lat;
    portEXIT_CRITICAL(&s_gps_mux);
    return v;
}

double gps_lon() {
    portENTER_CRITICAL(&s_gps_mux);
    double v = g_gps.lon;
    portEXIT_CRITICAL(&s_gps_mux);
    return v;
}

const char* gps_location_str() {
    static char buf[32];
    portENTER_CRITICAL(&s_gps_mux);
    memcpy(buf, g_gps.loc, sizeof(buf));
    portEXIT_CRITICAL(&s_gps_mux);
    return buf;
}

GpsSnapshot gps_snapshot() {
    GpsSnapshot s;
    portENTER_CRITICAL(&s_gps_mux);
    s.valid      = g_gps.valid;
    s.lat        = g_gps.lat;
    s.lon        = g_gps.lon;
    s.speed_kmh  = g_gps.speed_kmh;
    s.course_deg = g_gps.course_deg;
    memcpy(s.loc, g_gps.loc, sizeof(s.loc));
    portEXIT_CRITICAL(&s_gps_mux);
    return s;
}

// ── Log-Datei über Serial ausgeben (tail_lines=0 → alles) ──
static void serial_dump_log(const char* path, uint16_t tail_lines) {
    if (!spiffs_lock(2000)) {
        Serial.printf("[CMD] %s — SPIFFS busy\n", path);
        return;
    }
    if (!SPIFFS.exists(path)) {
        Serial.printf("[CMD] %s leer/nicht vorhanden\n", path);
        spiffs_unlock();
        return;
    }
    File f = SPIFFS.open(path, "r");
    uint32_t sz = f.size();
    Serial.printf("\n=== %s (%u Bytes) ===\n", path, sz);

    if (tail_lines == 0) {
        // Alles ausgeben, zeilenweise mit yield()
        char buf[256];
        while (f.available()) {
            int n = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
            buf[n] = '\0';
            Serial.println(buf);
            yield();
        }
    } else {
        // Nur letzte N Zeilen: erst Zeilen zählen
        uint32_t total = 0;
        while (f.available()) { if (f.read() == '\n') total++; }
        uint32_t skip = (total > tail_lines) ? total - tail_lines : 0;
        f.seek(0);
        uint32_t line = 0;
        char buf[256];
        while (f.available()) {
            int n = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
            buf[n] = '\0';
            if (line >= skip) Serial.println(buf);
            line++;
            yield();
        }
    }
    f.close();
    spiffs_unlock();
    Serial.println("=== Ende ===");
}

// ── Log-Befehl parsen: "syslog" oder "syslog 50" ──
static bool serial_log_cmd(const char* input, const char* cmd, const char* path) {
    size_t cmdlen = strlen(cmd);
    if (strncmp(input, cmd, cmdlen) != 0) return false;
    if (input[cmdlen] == '\0') {
        serial_dump_log(path, 0);
        return true;
    }
    if (input[cmdlen] == ' ') {
        uint16_t n = atoi(input + cmdlen + 1);
        serial_dump_log(path, n > 0 ? n : 0);
        return true;
    }
    return false;
}

void setup() {
    // USB CDC Timeout verkürzen — sonst wartet ESP32-S3 nach Deep Sleep
    // bis zu 60s auf USB-Host-Enumeration bevor setup() weiterläuft
    // Wake-Grund SOFORT als allererstes — vor Serial, delay, allem
    sleep_init();

    Serial.setTxTimeoutMs(0);
    Serial.begin(115200);
    delay(100);
    Serial.printf("\n=== Telemetry Stick v%s · CAN TX%d/RX%d %dkbps · I2C SDA%d/SCL%d ===\n",
                  FW_VERSION, CAN_TX_PIN, CAN_RX_PIN, CAN_SPEED_KBPS, RTC_SDA_PIN, RTC_SCL_PIN);

    logMutex = xSemaphoreCreateMutex();

    // 0. Laufzeit-Config aus NVS laden (vor web_init — softAP braucht SSID/Pass)
    cfg_init();

    // 1. WiFi + Web
    web_init();
    logs_init();
    { char m[64]; snprintf(m, sizeof(m), "AP: %s · http://192.168.4.1", cfg_ap_ssid()); syslog("WEB", m); }
    sleep_log_wakeup_syslog();  // Wake-Log jetzt senden (logs_init muss vorher laufen)
    ble_web_routes_init();

    // 2. I2C Sensoren
    syslog("BOOT", "rtc_init...");
    rtc_init();
    syslog("BOOT", "rtc_init OK");
    syslog("BOOT", "gyro_init...");
    gyro_init();
    syslog("BOOT", "gyro_init OK");
    sleep_log_wake();
    syslog("BOOT", "pmu_init...");
    pmu_init();
    syslog("BOOT", "pmu_init OK");
    gps_ext_wake();  // M10 sofort aus Backup wecken — sucht Sats während Rest bootet

    { int b = pmu_batt_pct();
      char msg[40];
      if (b >= 0) snprintf(msg, sizeof(msg), "Akku: %d%%", b);
      else        snprintf(msg, sizeof(msg), "Akku: nicht angeschlossen");
      syslog("PMU", msg);
    }
    if (!cfg_mod_gps()) {
        pmu_set_gps_power(false);  // BLDO2 (GPS-Antenne) abschalten
        syslog("GPS", "Modul deaktiviert (cfg mod_gps=0) — BLDO2 aus");
    }
    modem_init();        // UART1 (TX=GPIO5, RX=GPIO4) SIM7080G — übernimmt Modem exklusiv
    modem_start_task();  // FreeRTOS: GPS alle 5s + Traccar alle 60s

    // Telemetrie-Modul (nach Modem-Init — liest GPS/LTE-Daten)
    telem_init();

    // Zeit aus RTC übernehmen (VOR telem_restore — braucht korrekte RTC für Sleep-Zeit)
    {
        int h, m, s;
        if (rtc_get_time(h, m, s)) {
            char m2[32]; snprintf(m2, sizeof(m2), "Zeit übernommen: %02d:%02d:%02d", h, m, s); syslog("RTC", m2);
        } else {
            syslog("RTC", "Zeitübernahme fehlgeschlagen");
        }
    }
    rtc_time_sync_task_init();  // Sync sofort + periodischer Task

    telem_restore_from_spiffs();  // Phase 3: Daten aus Deep Sleep laden (jetzt mit korrekter RTC)
    telem_start_task();  // FreeRTOS: GPS/Gyro/LTE 10s, PMU 60s

    // WiFi upload — two priority slots (e.g. home WiFi + phone hotspot).
    // Empty SSID disables a slot. Stick scans + joins whichever is in range,
    // slot 0 preferred over slot 1 on ties.
    wifi_upload_configure_slot(0, cfg_sta_ssid(),   cfg_sta_pass(),   cfg_upload_url());
    wifi_upload_configure_slot(1, cfg_sta_ssid_2(), cfg_sta_pass_2(), cfg_upload_url_2());
    wifi_upload_start_task();

    // 3. AP hochfahren
    delay(2000);

    // 4. BLE
    ble_scan_init();
    delay(200);
    elm327_init();
    NimBLEDevice::startAdvertising();

    // 5. CAN
    gps_ext_init();      // Ext. GPS (BLITZ Mini M10, UART2) — nach CAN damit TWAI-Interrupts Vorrang haben
    if (!can_init(CAN_SPEED_KBPS)) {
        Serial.println("[CAN] FEHLER - Checklist:");
        Serial.println("  GPIO17→CTX, GPIO18→CRX");
        Serial.println("  3.3V→VCC, GND→GND");
        Serial.println("  CANH→OBD2 Pin6, CANL→OBD2 Pin14");
    }

    xTaskCreatePinnedToCore(monitor_task, "MON", 4096, NULL, 1, NULL, 0);

    syslog("SYS", "Bereit");

    // Heap + SPIFFS Status ins syslog
    { char msg[80];
      snprintf(msg, sizeof(msg), "Heap: %u KB frei, SPIFFS: %u/%u KB belegt",
               ESP.getFreeHeap() / 1024,
               (SPIFFS.usedBytes()) / 1024,
               (SPIFFS.totalBytes()) / 1024);
      syslog("SYS", msg);
    }
}

void loop() {
    // ── Serial-Befehle (Terminal) ──
    static char serial_buf[80];
    static uint8_t serial_pos = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serial_pos > 0) {
                serial_buf[serial_pos] = '\0';
                if (strcmp(serial_buf, "nosleep") == 0) {
                    g_nosleep = !g_nosleep;
                    Serial.printf("[CMD] Sleep %s (bis Neustart)\n",
                                  g_nosleep ? "DEAKTIVIERT" : "aktiviert");
                } else if (strcmp(serial_buf, "sleep") == 0) {
                    Serial.println("[CMD] → Deep Sleep");
                    sleep_force();
                } else if (strcmp(serial_buf, "gps") == 0) {
                    modem_print_gps_info();
                } else if (strcmp(serial_buf, "lte") == 0) {
                    modem_print_lte_info();
                } else if (strcmp(serial_buf, "lte sig") == 0) {
                    modem_print_lte_sig();
                } else if (strcmp(serial_buf, "lte scan") == 0) {
                    modem_print_lte_scan();
                } else if (strcmp(serial_buf, "lte bands") == 0) {
                    modem_print_lte_bands();
                } else if (strcmp(serial_buf, "lte bands fix") == 0) {
                    modem_lte_bands_fix(false);
                } else if (strcmp(serial_buf, "lte bands all") == 0) {
                    modem_lte_bands_fix(true);
                } else if (strcmp(serial_buf, "at stop") == 0) {
                    modem_pause_task();
                } else if (strcmp(serial_buf, "at start") == 0) {
                    modem_resume_task();
                } else if (strncmp(serial_buf, "at ", 3) == 0) {
                    // FIXES B.17: Raw-AT teilt sich die UART mit dem MODEM-Task →
                    // Task pausieren, sonst verschränken sich die Kommandos.
                    // Mit "at start" wieder aufnehmen.
                    modem_pause_task();
                    modem_send_at(serial_buf + 3);
                    Serial.println("[CMD] MODEM-Task pausiert für Raw-AT — 'at start' zum Fortsetzen");
                } else if (strcmp(serial_buf, "mqtt") == 0) {
                    mqtt_print_info();
                } else if (strcmp(serial_buf, "mqtt test") == 0) {
                    Serial.println("[CMD] MQTT Test-Publish...");
                    telem_force_capture("Test via Serial");
                    Serial.printf("[CMD] %u Zeilen ausstehend\n", telem_get_row_pending());
                } else if (strcmp(serial_buf, "mqtt drop") == 0) {
                    Serial.println("[CMD] MQTT + GPRS getrennt — simuliert Funkloch");
                    mqtt_disconnect();
                    modem_send_at("+CNACT=0,0");  // GPRS/PDP trennen
                } else if (strcmp(serial_buf, "can sniff") == 0) {
                    can_sniff(5000);  // 5 Sekunden
                } else if (strcmp(serial_buf, "gyro cal") == 0) {
                    Serial.println("[CMD] Gyro-Kalibrierung — Board muss still liegen!");
                    float baseline = 0, stddev = 0;
                    bool ok = gyro_recalibrate(&baseline, &stddev);
                    Serial.printf("[CMD] Gyro cal: %s · baseline=%.4f stddev=%.4f\n",
                                  ok ? "OK" : "FEHLER (zu viel Bewegung)", baseline, stddev);
                } else if (strcmp(serial_buf, "reset") == 0) {
                    Serial.println("[CMD] → Neustart");
                    Serial.flush();
                    delay(100);
                    esp_restart();
                } else if (serial_log_cmd(serial_buf, "syslog",  SPIFFS_SYS_LOG))  {
                } else if (serial_log_cmd(serial_buf, "elmlog",  SPIFFS_ELM_LOG))  {
                } else if (serial_log_cmd(serial_buf, "blelog",  SPIFFS_BLE_LOG))  {
                } else if (serial_log_cmd(serial_buf, "scanlog", SPIFFS_SCAN_LOG)) {
                } else if (strcmp(serial_buf, "clearlog") == 0) {
                    if (spiffs_lock(2000)) {
                        if (SPIFFS.exists(SPIFFS_SYS_LOG))  SPIFFS.remove(SPIFFS_SYS_LOG);
                        if (SPIFFS.exists(SPIFFS_ELM_LOG))  SPIFFS.remove(SPIFFS_ELM_LOG);
                        if (SPIFFS.exists(SPIFFS_BLE_LOG))  SPIFFS.remove(SPIFFS_BLE_LOG);
                        if (SPIFFS.exists(SPIFFS_SCAN_LOG)) SPIFFS.remove(SPIFFS_SCAN_LOG);
                        spiffs_unlock();
                        Serial.println("[CMD] Alle Logs geloescht");
                    } else {
                        Serial.println("[CMD] SPIFFS busy");
                    }
                } else if (strcmp(serial_buf, "help") == 0) {
                    Serial.println("Befehle: sleep, nosleep, gps, lte, lte scan, mqtt, mqtt drop, can sniff, reset");
                    Serial.println("  LTE:   lte bands, lte bands fix, lte bands all");
                    Serial.println("  Gyro:  gyro cal  (Kalibrierung — Board muss still liegen)");
                    Serial.println("  Debug: at +KOMMANDO  (roher AT-Befehl an Modem)");
                    Serial.println("  Logs:  syslog, elmlog, blelog, scanlog, clearlog");
                } else {
                    Serial.printf("[CMD] Unbekannt: '%s' → help\n", serial_buf);
                }
                serial_pos = 0;
            }
        } else if (serial_pos < sizeof(serial_buf) - 1) {
            serial_buf[serial_pos++] = c;
        }
    }

    ws.cleanupClients(1);  // max 1 WS-Client: alte Verbindung beim Seitenwechsel sofort schließen
    web_ap_update();       // AP-Timeout (2 min ohne Client) + VBUS-Restart
    sleep_update();        // Inaktivität → Deep Sleep

    // Minütlicher Status-Log
    static uint32_t s_last_batt_log_ms = 0;
    if ((millis() - s_last_batt_log_ms) >= 60000UL) {
        int b = pmu_batt_pct();
        char msg[80];
        if (b >= 0) snprintf(msg, sizeof(msg), "Akku: %d%% · Heap: %uKB · SPIFFS: %u/%uKB",
                             b, ESP.getFreeHeap()/1024, SPIFFS.usedBytes()/1024, SPIFFS.totalBytes()/1024);
        else        snprintf(msg, sizeof(msg), "Akku: n/a · Heap: %uKB · SPIFFS: %u/%uKB",
                             ESP.getFreeHeap()/1024, SPIFFS.usedBytes()/1024, SPIFFS.totalBytes()/1024);
        syslog("STATUS", msg);
        s_last_batt_log_ms = millis();
    }

    delay(100);
}
