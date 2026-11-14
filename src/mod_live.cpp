#include "mod_live.h"
#include "live_cbor.h"
#include "mod_ble_scan.h"
#include "mod_config.h"
#include "mod_logs.h"
#include "mod_rtc.h"
#include "mod_sleep.h"
#include "mod_telemetry.h"
#include "shared.h"
#include <NimBLEDevice.h>
#include <esp_random.h>

#if FEATURE_BLE

#define LIVE_MAX_SUBS      3
#define LIVE_DRIVE_MS      1000
#define LIVE_PARK_MS       30000
#define LIVE_FRESH_MS      120000   // aelter = nicht mehr "live", Feld entfaellt
#define LIVE_ODO_FRESH_MS  900000   // der km-Stand wird nur alle 60 s gelesen
#define LIVE_PAIR_GRACE_MS 60000    // so lange darf eine Verbindung ungekoppelt abonniert sein

static NimBLECharacteristic* s_char = nullptr;
static uint16_t s_subs[LIVE_MAX_SUBS];
static uint32_t s_sub_since[LIVE_MAX_SUBS];
static int      s_sub_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_seq = 0, s_sent = 0, s_skipped = 0, s_no_time = 0;
static volatile bool s_send_caps = false;

// ── Kopplungsfenster (siehe mod_live.h) ─────────────────────
static volatile bool     s_pair_open  = false;
static volatile uint32_t s_pair_start = 0;      // millis() beim Oeffnen
static int               s_pair_bonds = 0;      // gekoppelte Geraete beim Oeffnen

static uint32_t random_code() { return 100000 + (esp_random() % 900000); }

// Ausserhalb des Fensters: ein Code, den niemand kennt.
static void secret_passkey() { NimBLEDevice::setSecurityPasskey(random_code()); }

static void pair_close(const char* why) {
    if (!s_pair_open) return;
    s_pair_open = false;
    secret_passkey();
    syslog("LIVE", why);
}

uint32_t live_pair_seconds_left() {
    if (!s_pair_open) return 0;
    uint32_t gone = (millis() - s_pair_start) / 1000;
    return gone >= LIVE_PAIR_WINDOW_S ? 0 : LIVE_PAIR_WINDOW_S - gone;
}

static bool sub_secure(uint16_t conn) {
    ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return false;
    return d.sec_state.encrypted && d.sec_state.authenticated;
}

static void sub_remove(uint16_t conn) {
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_sub_count; i++) {
        if (s_subs[i] != conn) continue;
        s_subs[i] = s_subs[s_sub_count - 1];
        s_sub_since[i] = s_sub_since[s_sub_count - 1];
        s_sub_count--;
        break;
    }
    portEXIT_CRITICAL(&s_mux);
}

class LiveCharCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
        uint16_t conn = desc->conn_handle;
        sub_remove(conn);
        if (subValue == 0) return;
        portENTER_CRITICAL(&s_mux);
        bool room = s_sub_count < LIVE_MAX_SUBS;
        if (room) {
            s_subs[s_sub_count] = conn;
            s_sub_since[s_sub_count] = millis();
            s_sub_count++;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!room) return;
        if (!(desc->sec_state.encrypted && desc->sec_state.authenticated))
            NimBLEDevice::startSecurity(conn);      // Kopplung mit Code anstossen
        s_send_caps = true;
        syslog("LIVE", "Anzeigegeraet abonniert");
    }
};
static LiveCharCallbacks s_cb;

static bool fresh(TelemField f, float* v, uint32_t max_age) {
    uint32_t age;
    return telem_get_latest(f, v, &age) && age <= max_age;
}

static void collect(LiveValues& v) {
    memset(&v, 0, sizeof(v));
    float f, lat, lon, ok;
    v.has_soc      = fresh(TELEM_SOC, &v.soc, LIVE_FRESH_MS);
    v.has_range    = fresh(TELEM_RANGE, &v.range, LIVE_FRESH_MS);
    v.has_power    = fresh(TELEM_POWER, &v.power, LIVE_FRESH_MS);
    v.has_speed    = fresh(TELEM_VEHICLE_SPEED, &v.speed, LIVE_FRESH_MS);
    v.has_odometer = fresh(TELEM_ODOMETER, &v.odometer, LIVE_ODO_FRESH_MS);
    v.has_t_ext    = fresh(TELEM_EXT_TEMP, &v.t_ext, LIVE_FRESH_MS);
    v.has_t_batt   = fresh(TELEM_BATT_TEMP, &v.t_batt, LIVE_FRESH_MS);
    if ((v.has_charging = fresh(TELEM_IS_CHARGING, &f, LIVE_FRESH_MS))) v.charging = f > 0.5f;
    if ((v.has_dc = fresh(TELEM_IS_DCFC, &f, LIVE_FRESH_MS))) v.dc = f > 0.5f;
    if (fresh(TELEM_GPS_VALID, &ok, LIVE_FRESH_MS) && ok > 0.5f
        && fresh(TELEM_GPS_LAT, &lat, LIVE_FRESH_MS) && fresh(TELEM_GPS_LON, &lon, LIVE_FRESH_MS)) {
        v.has_pos = true;
        v.lat = lat;
        v.lon = lon;
    }
}

// Ein Telegramm an alle gekoppelten Abonnenten. Wer nach der Schonfrist noch
// ungekoppelt ist, wird getrennt.
static void send_all(const uint8_t* data, size_t len) {
    if (len == 0) return;
    uint16_t subs[LIVE_MAX_SUBS];
    uint32_t since[LIVE_MAX_SUBS];
    portENTER_CRITICAL(&s_mux);
    int n = s_sub_count;
    memcpy(subs, s_subs, sizeof(subs));
    memcpy(since, s_sub_since, sizeof(since));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        uint16_t conn = subs[i];
        ble_gap_conn_desc d;
        if (ble_gap_conn_find(conn, &d) != 0) { sub_remove(conn); continue; }
        if (!(d.sec_state.encrypted && d.sec_state.authenticated)) {
            if (millis() - since[i] > LIVE_PAIR_GRACE_MS) {
                syslog("LIVE", "ungekoppeltes Geraet getrennt");
                ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
                sub_remove(conn);
            }
            continue;
        }
        if (len + 3 > ble_att_mtu(conn)) { s_skipped++; continue; }   // passt nicht in eine Nachricht
        os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
        if (om == nullptr) { s_skipped++; continue; }
        int rc = ble_gattc_notify_custom(conn, s_char->getHandle(), om);
        if (rc == 0) s_sent++;
        else if (rc == BLE_HS_ENOTCONN) sub_remove(conn);
        else s_skipped++;
    }
}

static void live_task(void*) {
    uint32_t last_status = 0, last_pos = 0;
    uint8_t buf[200];
    while (!g_shutdown) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_pair_open && live_pair_seconds_left() == 0) pair_close("Kopplungsfenster abgelaufen");
        if (s_sub_count == 0) continue;
        uint32_t unix_s = (uint32_t)(rtc_unix_ms() / 1000ULL);
        if (unix_s == 0) {                  // ohne Zeit kein Telegramm (wie beim Speichern der Zeilen)
            s_no_time++;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        LiveValues v;
        collect(v);
        bool driving = v.has_speed && v.speed > 1.0f;
        uint32_t interval = driving ? LIVE_DRIVE_MS : LIVE_PARK_MS;
        uint32_t now = millis();
        if (s_send_caps) {
            s_send_caps = false;
            last_status = last_pos = 0;     // neuer Abonnent bekommt sofort einen Stand
            send_all(buf, live_encode_caps(v, unix_s, ++s_seq, interval / 1000, buf, sizeof(buf)));
        }
        if (last_status == 0 || now - last_status >= interval) {
            size_t n = live_encode_status(v, unix_s, s_seq + 1, buf, sizeof(buf));
            if (n) {
                s_seq++;
                s_char->setValue(buf, n);   // Read liefert den letzten Stand (ebenfalls nur gekoppelt)
                send_all(buf, n);
            }
            last_status = now;
        }
        if (v.has_pos && (last_pos == 0 || now - last_pos >= interval)) {
            size_t n = live_encode_position(v, unix_s, s_seq + 1, buf, sizeof(buf));
            if (n) {
                s_seq++;
                send_all(buf, n);
            }
            last_pos = now;
        }
    }
    vTaskDelete(NULL);
}

void live_init() {
    if (!cfg_live_ble()) return;
    NimBLEServer* server = ble_get_server();
    if (server == nullptr) return;
    // Kopplung: der Stick "zeigt" den Code (in den Einstellungen, nur im Kopplungsfenster),
    // das Geraet gibt ihn ein. Bis dahin gilt ein geheimer Zufallscode.
    NimBLEDevice::setSecurityAuth(true, true, true);            // Bonding, MITM-Schutz, Secure Connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    secret_passkey();
    NimBLEService* svc = server->createService(LIVE_SERVICE_UUID);
    s_char = svc->createCharacteristic(LIVE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
    s_char->setCallbacks(&s_cb);
    svc->start();
    NimBLEDevice::getAdvertising()->addServiceUUID(LIVE_SERVICE_UUID);
    xTaskCreatePinnedToCore(live_task, "live_ble", 4096, nullptr, 1, nullptr, 1);
    syslog("LIVE", "Live-Daten ueber Bluetooth bereit (nur gekoppelte Geraete)");
}

void live_status_str(char* buf, int size) {
    snprintf(buf, size, "{\"on\":%s,\"subs\":%d,\"sent\":%lu,\"skipped\":%lu,\"no_time\":%lu,\"pair_s\":%lu}",
             s_char ? "true" : "false", s_sub_count, (unsigned long)s_sent, (unsigned long)s_skipped,
             (unsigned long)s_no_time, (unsigned long)live_pair_seconds_left());
}

uint32_t live_pair_open() {
    if (s_char == nullptr) return 0;                // Dienst aus (oder noch nicht neu gestartet)
    uint32_t code = random_code();
    NimBLEDevice::setSecurityPasskey(code);
    s_pair_bonds = NimBLEDevice::getNumBonds();
    s_pair_start = millis();
    s_pair_open  = true;
    syslog("LIVE", "Kopplungsfenster offen (3 min, ein Versuch)");
    return code;
}

void live_auth_done(bool encrypted) {
    if (s_char == nullptr) return;
    if (!s_pair_open) {
        if (!encrypted) secret_passkey();           // Versuch ohne Fenster: der geheime Code gilt nie zweimal
        return;
    }
    if (!encrypted) pair_close("Kopplung fehlgeschlagen - Code verbraucht");
    else if (NimBLEDevice::getNumBonds() > s_pair_bonds) pair_close("Geraet gekoppelt - Kopplungsfenster zu");
    // sonst: ein schon gekoppeltes Geraet hat sich wieder verbunden, das Fenster bleibt offen
}

#else
void live_init() {}
void live_status_str(char* buf, int size) { snprintf(buf, size, "{\"on\":false}"); }
uint32_t live_pair_open() { return 0; }
uint32_t live_pair_seconds_left() { return 0; }
void live_auth_done(bool) {}
#endif
