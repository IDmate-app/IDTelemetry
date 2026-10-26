#include "mod_sleep.h"
#include "mod_gyro.h"
#include "mod_logs.h"
#include "mod_telemetry.h"
#include "mod_modem.h"
#include "mod_can.h"
#include "mod_pmu.h"
#include "shared.h"
#include "mod_gps_ext.h"
#include "config.h"
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_core_dump.h>
#include <driver/rtc_io.h>
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>

// ============================================================
//  mod_sleep - Deep Sleep Management
// ============================================================

#define SLEEP_NO_VBUS_MS   (5UL * 60UL * 1000UL)  // 5 min ohne VBUS → Sleep

// Globales Shutdown-Flag: wird von enter_deep_sleep() gesetzt.
// Alle FreeRTOS-Tasks prüfen dies in ihrer Loop und beenden sich sauber,
// damit shared Resources (I2C, CAN, WiFi) nicht im gesperrten Zustand bleiben.
volatile bool g_shutdown       = false;
volatile bool g_nosleep        = false;
volatile bool g_sleep_requested = false;
volatile bool g_trip_ending    = false;

static uint32_t g_boot_ms = 0;
static uint32_t g_last_vbus_seen_ms = 0;  // letzter Zeitpunkt mit VBUS
static esp_sleep_wakeup_cause_t g_wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

bool sleep_was_deep() {
    return g_wake_cause == ESP_SLEEP_WAKEUP_EXT0 ||
           g_wake_cause == ESP_SLEEP_WAKEUP_EXT1;
}

static void enter_deep_sleep(const char* reason);

// ── Init: Wake-Grund loggen ───────────────────────────────
void sleep_init() {
    // Wake-Cause SOFORT lesen — vor WiFi/delay/alles
    g_boot_ms    = millis();
    g_wake_cause = esp_sleep_get_wakeup_cause();
    // Serial-Output (syslog noch nicht verfügbar — kommt in sleep_log_wakeup_syslog)
    if (g_wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
        Serial.printf("[SLEEP] Aufgewacht: Gyro-Motion-Interrupt GPIO%d\n", (int)GYRO_WAKE_PIN);
    } else if (g_wake_cause == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.printf("[SLEEP] Aufgewacht: AXP2101 VBUS-Insert GPIO%d\n", (int)PMU_INT_PIN);
    }
}

// Syslog-Ausgabe des Wake-Grundes — nach logs_init() aufrufen
void sleep_log_wakeup_syslog() {
    if (g_wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
        syslog("WAKE", "Deep Sleep beendet: Gyro-Interrupt (GPIO3)");
    } else if (g_wake_cause == ESP_SLEEP_WAKEUP_EXT0) {
        syslog("WAKE", "Deep Sleep beendet: PMU VBUS-Insert (GPIO6)");
    } else {
        esp_reset_reason_t reason = esp_reset_reason();
        char msg[48];
        switch (reason) {
            case ESP_RST_PANIC:   snprintf(msg, sizeof(msg), "Neustart  Grund: PANIC/CRASH");   break;
            case ESP_RST_INT_WDT: snprintf(msg, sizeof(msg), "Neustart  Grund: WDT_INTERRUPT"); break;
            case ESP_RST_TASK_WDT:snprintf(msg, sizeof(msg), "Neustart  Grund: WDT_TASK");      break;
            case ESP_RST_WDT:     snprintf(msg, sizeof(msg), "Neustart  Grund: WDT");           break;
            case ESP_RST_BROWNOUT:snprintf(msg, sizeof(msg), "Neustart  Grund: BROWNOUT");      break;
            case ESP_RST_SW:      snprintf(msg, sizeof(msg), "Neustart  Grund: Software");      break;
            default:              snprintf(msg, sizeof(msg), "Normaler Start (PowerOn / Reset)"); break;
        }
        syslog("BOOT", msg);
        if (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
            syslog("BOOT", msg);  // doppelt loggen für Sichtbarkeit bei Crash

            // Coredump-Summary aus Flash-Partition lesen (falls vorhanden)
            esp_core_dump_summary_t* summary = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
            if (summary) {
                if (esp_core_dump_get_summary(summary) == ESP_OK) {
                    char cd[160];
                    snprintf(cd, sizeof(cd),
                        "PANIC Task:%s PC:0x%08lx EXC:%lu BT:",
                        summary->exc_task,
                        (unsigned long)summary->exc_pc,
                        (unsigned long)summary->ex_info.exc_cause);
                    // Backtrace PCs anhängen (bis max. 6 Frames)
                    size_t depth = summary->exc_bt_info.depth;
                    if (depth > 6) depth = 6;
                    for (size_t i = 0; i < depth; i++) {
                        char f[12];
                        snprintf(f, sizeof(f), "%08lx ", (unsigned long)summary->exc_bt_info.bt[i]);
                        strncat(cd, f, sizeof(cd) - strlen(cd) - 1);
                    }
                    syslog("PANIC", cd);
                    // Nach Auslesen löschen — nächster Crash bekommt frischen Slot
                    esp_core_dump_image_erase();
                }
                free(summary);
            }
        }
    }
}

// ── Nach gyro_init(): Wake-Details loggen ─────────────────
void sleep_log_wake() {
    if (g_wake_cause != ESP_SLEEP_WAKEUP_EXT1) return;
    char msg[140];
    float wake_g  = gyro_get_wake_accel();
    int   thr_mg  = gyro_get_mot_threshold() * 32;
    float delta_g = wake_g - 1.0f;  // Abweichung von Ruhelage (1G = Erdanziehung)
    snprintf(msg, sizeof(msg),
             "Gyro-Wake · Accel: %.3fG (delta %.0fmg) · HW-Schwelle: %dmg (MOT_THR=%d) · SW-Schwelle: %.0fmg",
             wake_g, delta_g * 1000.0f, thr_mg, gyro_get_mot_threshold(),
             gyro_get_threshold() * 1000.0f);
    syslog("WAKE", msg);
}

// ── Update: im loop() aufrufen ────────────────────────────
void sleep_update() {
    if (g_nosleep || g_shutdown) return;

    static bool s_trigger_logged = false;
    uint32_t now = millis();

    // ─── VBUS = Auto an → wach bleiben ─────────────────────
    bool vbus = pmu_is_vbus_in();
    if (vbus) {
        g_last_vbus_seen_ms = now;
        // Sleep-Anforderung zurücknehmen falls VBUS zurückgekehrt ist
        if (g_sleep_requested) {
            g_sleep_requested = false;
            g_trip_ending = false;
            s_trigger_logged = false;  // TRIGGER-Log bei erneutem Sleep wieder erlauben
            syslog("SLEEP", "Sleep abgebrochen — VBUS wieder da");
        }
        return;
    }

    // FIXES B.9: Unterspannungsschutz. Akku kritisch UND kein VBUS → sofort
    // schlafen (nicht erst nach 5/10 min). Zusammen mit dem VBUS-only-Wake in
    // enter_deep_sleep() bricht das die Gyro-Wake-Schleife beim Transport, die
    // den Akku sonst bis zur AXP2101-Hardware-Abschaltung leerzykelt.
    int batt_pct = pmu_batt_pct();
    bool batt_critical = (batt_pct >= 0 && batt_pct < SLEEP_LOW_BATT_PCT);
    if (batt_critical) {
        enter_deep_sleep("Deep Sleep: Akku kritisch, kein VBUS — nur VBUS-Wake");
        return;  // (erreicht enter_deep_sleep nicht zurück)
    }

    // VBUS weg → 5 min Schonfrist (Kurzunterbrechung, Tankpause)
    uint32_t vbus_gone_ms = now - g_last_vbus_seen_ms;
    // FIXES B.10: Bei defekter PMU liefert pmu_is_vbus_in() IMMER false → der
    // VBUS-Timer würde mitten in der Fahrt einschlafen (Gyro weckt sofort →
    // Dauerschleife, Modem-Boot-Storm). Ohne verlässliches VBUS-Signal NICHT
    // über den VBUS-Timer schlafen; dann entscheidet nur die Gyro-Inaktivität.
    bool vbus_sleep = pmu_ok() && (vbus_gone_ms >= SLEEP_NO_VBUS_MS);

    // Gyro-Fallback: kein VBUS aber Bewegung → wach bleiben (z.B. Transport)
    // Erst prüfen wenn VBUS mindestens 15s weg ist — kurze Glitches ignorieren.
    bool gyro_sleep = false;
    if (!vbus_sleep && vbus_gone_ms >= 15000UL && gyro_ok()) {
        uint32_t last_shake = gyro_last_shake_ms();
        uint32_t idle_since = (last_shake > 0) ? last_shake : g_boot_ms;
        // Race Condition: Gyro-Task kann last_shake nach now aktualisieren
        // → uint32_t Underflow → sofort gyro_sleep. Abfangen:
        if (idle_since <= now) {
            gyro_sleep = (now - idle_since) >= SLEEP_INACTIVITY_MS;
        }
    }

    if (!vbus_sleep && !gyro_sleep) return;

    // ─── Debug: warum wird Sleep ausgelöst? (nur einmal loggen) ──
    if (!s_trigger_logged) {
        s_trigger_logged = true;
        uint32_t last_shake = gyro_last_shake_ms();
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "TRIGGER: vbus=%d vbus_sleep=%d gyro_sleep=%d vbus_gone=%lums last_shake=%lu boot=%lu now=%lu",
                 vbus, vbus_sleep, gyro_sleep, vbus_gone_ms, last_shake, g_boot_ms, now);
        syslog("SLEEP", dbg);
    }

    // ─── Sleep-Schwelle erreicht ─────────────────────────
    // Ghost-Client-Erkennung: softAPgetStationNum() kann Stationen halten die
    // sich ohne Deauth getrennt haben (Handy-Screen gesperrt, WiFi kurz weg).
    // Echter Client = AP-Station UND aktiver WebSocket. Ghost = AP-Station aber
    // kein WebSocket → nach 3 min ignorieren.
    static uint32_t s_last_client_ms  = 0;  // letzter Zeitpunkt mit echtem Client
    static uint32_t s_ghost_since_ms  = 0;  // wann Ghost-Zustand begann
    {
        bool ap_station = (WiFi.softAPgetStationNum() > 0);
        bool ws_active  = (ws.count() > 0);
        bool real_client = ap_station && ws_active;
        bool ghost       = ap_station && !ws_active;

        if (real_client) {
            s_last_client_ms = now;
            s_ghost_since_ms = 0;
            static uint32_t last_log_ms = 0;
            if (now - last_log_ms >= 60000UL) {
                last_log_ms = now;
                syslog("SLEEP", "Sleep verhindert: Client verbunden");
            }
            return;
        }

        if (ghost) {
            if (s_ghost_since_ms == 0) s_ghost_since_ms = now;
            // Ghost-Timeout: 3 min ohne echten WebSocket → ignorieren
            if (now - s_ghost_since_ms < 3UL * 60UL * 1000UL) {
                static uint32_t last_ghost_log = 0;
                if (now - last_ghost_log >= 60000UL) {
                    last_ghost_log = now;
                    syslog("SLEEP", "Ghost-Client (kein WebSocket) · ignoriert");
                }
                return;
            }
            // Ghost-Timeout abgelaufen → sleep erlauben
        } else {
            s_ghost_since_ms = 0;
        }
    }

    // Schonfrist: 30s nach letztem echten Client warten (Browser-Seitenwechsel)
    if (s_last_client_ms > 0 && (now - s_last_client_ms) < 30000UL) return;

    // ─── Kein Client → Deep Sleep ───────────────────────
    char msg[96];
    if (vbus_sleep) {
        snprintf(msg, sizeof(msg), "Deep Sleep: VBUS weg seit %lu min",
                 SLEEP_NO_VBUS_MS / 60000UL);
    } else {
        snprintf(msg, sizeof(msg), "Deep Sleep: %lu min keine Gyro-Bewegung",
                 SLEEP_INACTIVITY_MS / 60000UL);
    }
    // Modem-Task soll Flush durchführen, dann schläft er selbst ein.
    // Falls Modem-Task nicht in STATE_RUNNING (z.B. Netzsuche), reagiert er nie →
    // nach 30s Timeout direkt einschlafen.
    static uint32_t s_sleep_req_ms = 0;
    if (!g_sleep_requested) {
        syslog("SLEEP", msg);
        g_sleep_requested = true;
        s_sleep_req_ms = now;
    } else if ((now - s_sleep_req_ms) >= 120000UL) {
        // Modem hat 2 min nicht reagiert (z.B. Netzsuche) → direkt einschlafen
        enter_deep_sleep(msg);
    }
}

// ── Force Sleep: sofort einschlafen (Serial-Befehl / Debug) ──
void sleep_force() {
    enter_deep_sleep("Deep Sleep: Shutdown abgeschlossen");
}

// ── Hilfsfunktion: prüfen ob ein Task noch existiert ──
static bool task_alive(const char* name) {
    return xTaskGetHandle(name) != nullptr;
}

// ── Gemeinsamer Sleep-Ablauf ─────────────────────────────
static void enter_deep_sleep(const char* reason) {
    syslog("SLEEP", reason);

    // ── 1. Telemetrie sichern (noch im laufenden System) ──
    telem_persist_to_spiffs();

    // ── 2. Shutdown-Signal an alle Tasks ──
    //    Tasks prüfen g_shutdown in ihrer Loop und beenden sich sauber,
    //    damit I2C/CAN/WiFi-Transaktionen ordentlich abgeschlossen werden.
    g_shutdown = true;

    // Warten bis alle Tasks sich beendet haben (max 2s)
    const char* task_names[] = {
        "TELEM", "MODEM", "GYRO", "WIFI_SCAN", "AP_MON", "MON", "RTC_SYNC", "elm_worker", "GPS_EXT",
        "WIFI_UP"   // FIXES B.3: muss VOR esp_wifi_stop() beendet werden, sonst Panic beim Sleep
    };
    uint32_t wait_start = millis();
    // Warte auf Tasks — NICHT auf den aktuell ausführenden Task warten
    // (enter_deep_sleep() kann vom MODEM-Task aufgerufen werden)
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    bool all_stopped = false;
    while (millis() - wait_start < 2000) {
        all_stopped = true;
        for (auto name : task_names) {
            TaskHandle_t h = xTaskGetHandle(name);
            if (h && h != caller) { all_stopped = false; break; }
        }
        if (all_stopped) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (all_stopped) {
        syslog("SLEEP", "Deep Sleep: Shutdown abgeschlossen");
    } else {
        syslog("SLEEP", "WARNUNG: Task-Timeout — erzwinge Stop");
        for (auto name : task_names) {
            TaskHandle_t h = xTaskGetHandle(name);
            if (h && h != caller) vTaskDelete(h);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ── 3. Wake-Pins vorab prüfen (noch vor WiFi-Shutdown → syslog geht raus) ──
    {
        int gyro_lvl = digitalRead(GYRO_WAKE_PIN);
        int pmu_lvl  = digitalRead(PMU_INT_PIN);
        char pin_msg[96];
        snprintf(pin_msg, sizeof(pin_msg),
                 "Wake-Pins: GPIO%d(Gyro)=%s  GPIO%d(PMU)=%s",
                 (int)GYRO_WAKE_PIN, gyro_lvl ? "HIGH!" : "LOW",
                 (int)PMU_INT_PIN,   pmu_lvl  ? "HIGH"  : "LOW!");
        syslog("SLEEP", pin_msg);
    }

    // Laden im Sleep: an lassen wenn VBUS vorhanden, sonst deaktivieren
    if (pmu_is_vbus_in()) {
        pmu_set_charging(true);
        syslog("SLEEP", "VBUS vorhanden · Laden bleibt aktiv im Sleep");
    } else {
        pmu_set_charging(false);
    }

    // Ext. GPS in Backup-Mode (µA) — behält Orbit-Daten + RTC, wacht per UART auf
    // DC5 bleibt AN damit Backup-Mode funktioniert (V_BCKP über Farad-Cap)
    gps_ext_sleep();
    modem_clear_reboot_count();   // FIXES B.17: WD-Reboot-Counter nicht über Sleep schleppen
    pmu_set_modem_power(false);
    can_stop();

    // BLE sicher stoppen (kein deinit — NimBLE-interne Tasks crashen sonst)
    NimBLEDevice::stopAdvertising();
    NimBLEServer* pSrv = NimBLEDevice::getServer();
    if (pSrv && pSrv->getConnectedCount() > 0) {
        pSrv->disconnect(0);
    }
    delay(100);

    // WiFi komplett aus — esp_wifi_stop() statt WiFi.mode(WIFI_OFF),
    // da mode(WIFI_OFF) nach Force-Kill des WIFI_SCAN-Tasks hängen kann.
    WiFi.scanDelete();
    WiFi.softAPdisconnect(false);
    esp_wifi_stop();

    Serial.flush();
    delay(100);

    // ── 4. Wake-up Quellen konfigurieren ──
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // RTC-Peripherie an

    // FIXES B.9: Bei kritischem Akku ohne VBUS den Gyro-Wake (EXT1) NICHT
    // aktivieren — sonst weckt jede Erschütterung beim Transport das Gerät,
    // jeder Wake = Minuten Netzsuche bei vollem Sendestrom → Akku zyklisch bis
    // zur AXP2101-Hardware-Abschaltung leer. Dann nur noch VBUS-Wake (EXT0):
    // das Gerät bleibt aus, bis es wieder Strom bekommt.
    int  ed_batt = pmu_batt_pct();
    bool ed_batt_critical = (ed_batt >= 0 && ed_batt < SLEEP_LOW_BATT_PCT) && !pmu_is_vbus_in();
    if (ed_batt_critical) {
        syslog("SLEEP", "Akku kritisch → Gyro-Wake AUS, nur VBUS-Wake");
    } else {
        // EXT1: GPIO3 HIGH → Gyro Motion Interrupt
        gyro_configure_sleep_int();
        rtc_gpio_init(GYRO_WAKE_PIN);
        rtc_gpio_set_direction(GYRO_WAKE_PIN, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_en(GYRO_WAKE_PIN);
        rtc_gpio_pullup_dis(GYRO_WAKE_PIN);
        esp_sleep_enable_ext1_wakeup(1ULL << GYRO_WAKE_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
    }

    // EXT0: GPIO6 LOW → AXP2101 VBUS-Insert Interrupt (active-low)
    pmu_enable_vbus_wake();
    rtc_gpio_init(PMU_INT_PIN);
    rtc_gpio_set_direction(PMU_INT_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(PMU_INT_PIN);     // Pull-Up: Pin ist HIGH wenn kein INT
    rtc_gpio_pulldown_dis(PMU_INT_PIN);
    esp_sleep_enable_ext0_wakeup(PMU_INT_PIN, 0);  // 0 = wake on LOW

    // Debug: DC5-Status vor Deep Sleep prüfen
    { char m[64]; snprintf(m, sizeof(m), "DC5 vor Sleep: %s", pmu_is_dc5_on() ? "AN" : "AUS!"); syslog("SLEEP", m); }

    Serial.flush();
    delay(100);

    esp_deep_sleep_start();

    // ── Fallback: wenn esp_deep_sleep_start() nicht greift → Neustart ──
    syslog("SLEEP", "FEHLER: Deep Sleep fehlgeschlagen — Neustart!");
    Serial.flush();
    delay(100);
    ESP.restart();
}
