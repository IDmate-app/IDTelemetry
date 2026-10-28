#pragma once
#include "secrets.h"

// ============================================================
//  Telemetry Stick — Zentrale Konfiguration
//  Hardware: LILYGO T-SIM7080G-S3 (ESP32-S3-WROOM-1-N16R8) + SN65HVD230
// ============================================================

#define FW_VERSION  "1.4.0"

// ============================================================
//  BUILD-VARIANT FEATURE FLAGS
//  Defined by the active env in platformio.ini. Defaults below
//  keep direct compilation (without -DFEATURE_*) behaving like
//  the historical s3_full build.
// ============================================================
#ifndef FEATURE_MODEM
#define FEATURE_MODEM       1
#endif
#ifndef FEATURE_GPS_EXT
#define FEATURE_GPS_EXT     1
#endif
#ifndef FEATURE_PMU
#define FEATURE_PMU         1
#endif
#ifndef FEATURE_BLE
#define FEATURE_BLE         1
#endif
#ifndef FEATURE_CAN
#define FEATURE_CAN         1
#endif
#ifndef FEATURE_WIFI_UPLOAD
#define FEATURE_WIFI_UPLOAD 0
#endif

// ============================================================
//  MODUL-SCHALTER  ← hier ein-/ausschalten
// ============================================================
#define GPS_EXT_ENABLED     true   // Externes u-blox GPS an UART2, Versorgung PMU-Rail DC5
#define GPS_EXT_RX_PIN       1     // GPIO1  ← GPS TX
#define GPS_EXT_TX_PIN       2     // GPIO2  → GPS RX
#define GPS_EXT_BAUD         38400   // Werkseinstellung des DAN-F10N und erster Kandidat
                                   // der Baudsuche. Stand hier 115200, was bei jedem
                                   // Kaltstart ~1,2 s verschenkte: der erste Versuch
                                   // musste erst in den Timeout laufen. gps_ext_init()
                                   // erkennt die Rate ohnehin selbst.
                                   // false = nur internes SIM7080G-GPS + LTE

// ELM327 Mock-Modus: Antwortet auf alle OBD-Anfragen mit Dummy-Werten
// #define ELM_MOCK_MODE

// LTE deaktivieren (GPS-Only Modus, keine SIM vorhanden)
// #define LTE_DISABLED

// ============================================================
//  GPS Capture-Schwellen
// ============================================================
//  Distanz-Trigger: geschwindigkeitsabhängig (berechnet in mod_telemetry)
//    <50 km/h → 100m, 50-80 → 150m, 80-110 → 200m, >110 → 250m
#define TELEM_GPS_MAX_INTERVAL_MS 60000UL // Zeit-Trigger: max. 1 Minute ohne Punkt
#define TELEM_GPS_MIN_SPEED_KMH    3.0f   // Zeit-Trigger nur ab dieser Geschwindigkeit
#define TELEM_YAW_TURN_DPS          6     // Kurven-Trigger (Gyro/Yaw): Drehrate in °/s
#define TELEM_CURVE_COOLDOWN_MS  2000UL   // Kurven-Cooldown: kein neuer Kurven-Trigger für 3s

// ============================================================
//  Telemetrie / Senden
// ============================================================
#define TELEM_ROW_BUF_SIZE       500    // max. Zeilen im RAM-Ringpuffer

// ============================================================
//  MQTT (persistente Verbindung via SIM7080G nativen Client)
// ============================================================
#define MQTT_KEEPALIVE_S        30      // Keep-Alive Ping (30s hält o2 CG-NAT offen, war 60s)
#define MQTT_QOS                1       // QoS 1 = at least once (Broker sendet PUBACK)
#define MQTT_RECONNECT_MS       10000UL // Reconnect-Intervall bei Verbindungsverlust
#define MQTT_PUBLISH_TIMEOUT_MS 10000UL // Timeout für Publish
#define MQTT_FAIL_RESET_COUNT   3       // Nach N Fehlversuchen → Modem-Reset (PWRKEY)
#define MODEM_RESET_ESCALATE    2       // Nach N Modem-Resets ohne Erfolg → PLMN-Scan
#define PLMN_SCAN_ESCALATE      2       // Nach N PLMN-Scans ohne Erfolg → Reboot
#define WD_REBOOT_MAX           3       // Max Reboots innerhalb WD_REBOOT_WINDOW_S
#define WD_REBOOT_WINDOW_S      1800    // 30 Minuten Fenster fuer Reboot-Limiter

// Bevorzugter Provider: wird bei jeder Registrierung zuerst versucht.
// Telekom DE (26201) statt o2 (26203), weil o2 auf dem Land unzuverlaessig ist.
// Leer lassen ("") fuer reinen Auto-Modus.
#define PREFERRED_PLMN          "26201"
#define PREFERRED_PLMN_TIMEOUT_S 15     // Max Wartezeit fuer Praeferenz (sonst Fallback)

// ============================================================
//  Deep Sleep / Gyro
// ============================================================
#define GYRO_WAKE_PIN           GPIO_NUM_3              // MPU-6050 INT → GPIO3 (EXT1 Wake-up)
#define SLEEP_INACTIVITY_MS     (10UL * 60UL * 1000UL) // 10 Minuten Inaktivität
#define SLEEP_LOW_BATT_PCT      10      // FIXES B.9: unter dieser Akku-% ohne VBUS → sofort Sleep, nur VBUS-Wake
#define GYRO_SHAKE_THRESHOLD    0.05f   // 0.05G = deutliche Bewegung → gelb
#define GYRO_HOLD_MS            30000   // SHAKE bleibt 30s aktiv

// ============================================================
//  WiFi / Access Point
// ============================================================
#define AP_TIMEOUT_MS    (2UL * 60UL * 1000UL)  // 2 min ohne Client → AP aus

// ============================================================
//  Log
// ============================================================
#define MAX_LOG_ENTRIES     200   // RAM Ring-Buffer
#define LOG_INFO_LEN         48   // Max Info-String pro Eintrag
#define LOG_CAN_ENABLED_DEFAULT   false
#define LOG_BLE_ENABLED_DEFAULT   false
#define LOG_WIFI_ENABLED_DEFAULT  false

// ============================================================
//  OBD Timeouts
// ============================================================
#define OBD_TIMEOUT_MS      300
#define UDS_TIMEOUT_MS      400
#define SCAN_DELAY_MS        80

// ============================================================
//  Hardware-Pins (fest verdrahtet, normalerweise nicht ändern)
// ============================================================

// WiFi Access Point
#define WIFI_SSID           SECRET_AP_SSID
#define WIFI_PASS           SECRET_AP_PASS

// CAN / SN65HVD230
#define CAN_TX_PIN          GPIO_NUM_17
#define CAN_RX_PIN          GPIO_NUM_18
#define CAN_RS_PIN          -1           // -1 = RS am Modul auf GND gebrückt
#define CAN_SPEED_KBPS      500

// I2C Bus (extern): RTC DS1307 (0x68) + MPU-6050 (0x69)
// Das externe GPS (u-blox DAN-F10N) haengt an UART2, nicht am I2C.
#define RTC_SDA_PIN         45
#define RTC_SCL_PIN         21

// I2C Bus (intern): AXP2101 PMU (auf PCB verdrahtet, kein Lötpad)
#define PMU_SDA_PIN         15
#define PMU_SCL_PIN          7
#define PMU_INT_PIN     GPIO_NUM_6

// SIM7080G Modem / internes GPS (intern verdrahtet)
#define MODEM_TX_PIN        5
#define MODEM_RX_PIN        4
#define MODEM_PWRKEY_PIN    41
#define MODEM_DTR_PIN       42
#define MODEM_STATUS_PIN    40
#define MODEM_FLIGHT_PIN    44   // SIM7080G FLIGHT/RFKILL — LOW = RF aktiv (intern verdrahtet)
#define MODEM_BAUD          115200
#define GPS_INTERVAL_MS     5000
