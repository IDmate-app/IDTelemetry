#include "mod_gps_ext.h"
#include "mod_logs.h"
#include "mod_sleep.h"
#include "mod_rtc.h"
#include "config.h"
#include "shared.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <string.h>

// ============================================================
//  mod_gps_ext — externes u-blox GPS an UART2
//  Pins: GPIO1 = RX (← GPS TX), GPIO2 = TX (→ GPS RX), siehe config.h
//  Versorgung: PMU-Rail DC5 (3,3 V)
//  Die Baudrate wird beim Start ermittelt — u-blox liefert ab Werk 38400,
//  ältere Module 9600. GPS_EXT_BAUD ist nur der erste Kandidat.
//  UBX-Assist: Zeit (RTC) + letzte Position (NVS) beim Start injizieren
// ============================================================

static HardwareSerial s_gps_uart(2);  // UART2 (UART0=USB-CDC, UART1=Modem)
static bool  s_ok        = false;
static int   s_sat_count = -1;

// Erkannte Baudrate. Übersteht Deep Sleep, damit nach jedem Wake nicht
// erneut durchprobiert werden muss (das kostet sonst mehrere Sekunden).
RTC_DATA_ATTR static uint32_t s_baud = 0;

// ── UBX-Paket senden ─────────────────────────────────────
static void ubx_send(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    uint8_t ck_a = 0, ck_b = 0;
    auto sc = [&](uint8_t b) {
        ck_a += b; ck_b += ck_a;
        s_gps_uart.write(b);
    };
    s_gps_uart.write(0xB5);
    s_gps_uart.write(0x62);
    sc(cls); sc(id);
    sc((uint8_t)(len & 0xFF)); sc((uint8_t)(len >> 8));
    for (uint16_t i = 0; i < len; i++) sc(payload[i]);
    s_gps_uart.write(ck_a);
    s_gps_uart.write(ck_b);
}

// ── Auf UBX-ACK-ACK / ACK-NAK warten ─────────────────────
// Beide sind fest 8 Byte lang: B5 62 05 <01|00> 02 00 <cls> <id>.
// Deshalb reicht ein Schiebefenster über den Bytestrom — NMEA-Zeilen,
// die zwischendurch hereinlaufen, stören dabei nicht.
// Rückgabe: 1 = ACK, 0 = NAK (Modul lehnt ab), -1 = keine Antwort.
static int ubx_wait_ack(uint8_t cls, uint8_t id, uint32_t timeout_ms) {
    uint8_t  w[8]  = {};
    uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        if (!s_gps_uart.available()) { delay(1); continue; }
        memmove(w, w + 1, 7);
        w[7] = (uint8_t)s_gps_uart.read();
        if (w[0] == 0xB5 && w[1] == 0x62 && w[2] == 0x05 &&
            (w[3] == 0x01 || w[3] == 0x00) &&
            w[4] == 0x02 && w[5] == 0x00 &&
            w[6] == cls  && w[7] == id) {
            return (w[3] == 0x01) ? 1 : 0;
        }
    }
    return -1;
}

// Sendet einen Konfigurationsbefehl und protokolliert, was das Modul dazu sagt.
// Wichtig bei CFG-VALSET: der Befehl ist atomar — ein einziges vom Modul nicht
// unterstütztes Key-Value-Paar lässt die komplette Nachricht durchfallen.
// Ohne diese Prüfung scheitert das lautlos.
static bool ubx_send_checked(const char* what, uint8_t cls, uint8_t id,
                             const uint8_t* payload, uint16_t len) {
    while (s_gps_uart.available()) s_gps_uart.read();   // Altdaten verwerfen
    ubx_send(cls, id, payload, len);
    int rc = ubx_wait_ack(cls, id, 1200);
    char m[96];
    snprintf(m, sizeof(m), "%s: %s", what,
             rc == 1 ? "ACK" :
             rc == 0 ? "NAK — vom Modul abgelehnt" : "keine Antwort (Timeout)");
    syslog("GPS_EXT", m);
    return rc == 1;
}

// ── UBX-MGA-INI-TIME_UTC injizieren ──────────────────────
static void inject_time() {
    int y, mo, d, h, mi, s;
    if (!rtc_get_datetime(y, mo, d, h, mi, s)) return;
    // Plausibilisieren: DS1307 liefert gelegentlich I2C-Glitches mit unsinnigen Werten
    // (Jahr 2093, Monat 25 etc.). Nur akzeptieren wenn komplett plausibel.
    if (y < 2024 || y > 2040) return;
    if (mo < 1 || mo > 12) return;
    if (d  < 1 || d  > 31) return;
    if (h  < 0 || h  > 23) return;
    if (mi < 0 || mi > 59) return;
    if (s  < 0 || s  > 59) return;

    uint8_t p[24] = {};
    p[0]  = 0x10;           // type: TIME_UTC
    p[3]  = 18;             // aktuelle GPS-UTC Schaltsekunden
    p[4]  = (uint8_t)(y & 0xFF);
    p[5]  = (uint8_t)(y >> 8);
    p[6]  = (uint8_t)mo;
    p[7]  = (uint8_t)d;
    p[8]  = (uint8_t)h;
    p[9]  = (uint8_t)mi;
    p[10] = (uint8_t)s;
    p[16] = 60; p[17] = 0;  // tAccS = 60s — toleriert ungenaue RTC

    ubx_send(0x13, 0x40, p, 24);
    char msg[48];
    snprintf(msg, sizeof(msg), "UBX Zeit: %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    syslog("GPS_EXT", msg);
}


// ── Aktive Antenne einschalten (UBX-CFG-VALSET, M10) ─────
// M10 verwendet neues Konfig-Interface; CFG-ANT (0x06/0x13) ist M8-Legacy.
// CFG-HW-ANT_CFG_VOLTCTRL = 0x10A3002E → 1 = Antennenspannung an
static void enable_active_antenna() {
    uint8_t p[9] = {};
    p[0] = 0x00;              // version = 0
    p[1] = 0x01;              // layers: Bit0=RAM, Bit1=BBR, Bit2=Flash
                              // (war 0x00 = gar keine Ebene → Befehl lief ins Leere)
    p[2] = 0x00; p[3] = 0x00; // reserved
    // Key-Value-Paar: CFG-HW-ANT_CFG_VOLTCTRL (U1)
    p[4]  = 0x2E; p[5]  = 0x00; p[6]  = 0xA3; p[7]  = 0x10;  // Key (little-endian)
    p[8]  = 0x01;              // Value = 1 (ein)
    ubx_send_checked("Antennenspannung", 0x06, 0x8A, p, 9);
}

// ── AssistNow Autonomous (AOP) aktivieren ────────────────
// M10: UBX-CFG-VALSET mit CFG-ANA-USE_ANA (0x10230001) = 1
// Modul sammelt Satellitendaten und erstellt eigene Orbit-Vorhersagen
static void enable_aop() {
    uint8_t p[9] = {};
    p[0] = 0x00;              // version
    p[1] = 0x01;              // layers: RAM
    p[2] = 0x00; p[3] = 0x00; // reserved
    // CFG-ANA-USE_ANA = 0x10230001 (U1)
    p[4] = 0x01; p[5] = 0x00; p[6] = 0x23; p[7] = 0x10;  // Key
    p[8] = 0x01;              // Value = 1
    ubx_send_checked("AssistNow Autonomous", 0x06, 0x8A, p, 9);
}

// ── Alle 4 GNSS-Konstellationen aktivieren ──────────────
// M10 unterstützt GPS + GLONASS + Galileo + BeiDou gleichzeitig
// UBX-CFG-VALSET: mehrere Key-Value-Paare in einem Befehl
// Bewusst ein VALSET je Konstellation statt aller vier in einer Nachricht:
// CFG-VALSET ist atomar. Kennt ein Modul einen der Schlüssel nicht, verwirft
// es die komplette Nachricht — also auch die Konstellationen, die es könnte.
// Einzeln gesendet kostet ein abgelehnter Schlüssel nur sich selbst.
static void enable_all_gnss() {
    static const struct { const char* name; uint8_t key[4]; } sig[] = {
        { "GPS",     { 0x1F, 0x00, 0x31, 0x10 } },  // CFG-SIGNAL-GPS_ENA
        { "GLONASS", { 0x25, 0x00, 0x31, 0x10 } },  // CFG-SIGNAL-GLO_ENA
        { "Galileo", { 0x21, 0x00, 0x31, 0x10 } },  // CFG-SIGNAL-GAL_ENA
        { "BeiDou",  { 0x22, 0x00, 0x31, 0x10 } },  // CFG-SIGNAL-BDS_ENA
    };
    char ok[72] = {};
    for (size_t i = 0; i < sizeof(sig) / sizeof(sig[0]); i++) {
        uint8_t p[9] = {};
        p[0] = 0x00;              // version
        p[1] = 0x03;              // layers: RAM + BBR (überlebt Backup-Sleep)
        p[2] = 0x00; p[3] = 0x00; // reserved
        memcpy(p + 4, sig[i].key, 4);
        p[8] = 0x01;              // Value = 1 (ein)
        if (ubx_send_checked(sig[i].name, 0x06, 0x8A, p, 9)) {
            if (ok[0]) strncat(ok, "+", sizeof(ok) - strlen(ok) - 1);
            strncat(ok, sig[i].name, sizeof(ok) - strlen(ok) - 1);
        }
    }
    char m[96];
    snprintf(m, sizeof(m), "GNSS aktiv: %s", ok[0] ? ok : "(keine bestaetigt!)");
    syslog("GPS_EXT", m);
}

// ── TIMEPULSE ein/aus (PPS-LED) ──────────────────────────
// Nur RAM — M10 läuft durch, kein BBR nötig
static void set_timepulse(bool on) {
    uint8_t p[9] = {};
    p[0] = 0x00;
    p[1] = 0x01;              // nur RAM
    p[2] = 0x00; p[3] = 0x00;
    p[4] = 0x07; p[5] = 0x00; p[6] = 0x05; p[7] = 0x10;
    p[8] = on ? 0x01 : 0x00;
    ubx_send(0x06, 0x8A, p, 9);
}
static void disable_timepulse() { set_timepulse(false); }
static void enable_timepulse()  { set_timepulse(true);  }

// ── NMEA-Koordinate DDMM.MMMM → Dezimalgrad ──────────────
static double nmea_deg(const char* s, char dir) {
    if (!s || s[0] == '\0') return 0.0;
    double val = atof(s);
    int    d   = (int)(val / 100);
    double min = val - d * 100.0;
    double dec = d + min / 60.0;
    if (dir == 'S' || dir == 'W') dec = -dec;
    return dec;
}

static const char* nmea_field(char* buf, int idx) {
    int i = 0; char* p = buf;
    while (*p) { if (*p++ == ',') if (++i == idx) return p; }
    return "";
}

static bool nmea_checksum_ok(const char* line) {
    const char* s = line + 1;
    uint8_t csum = 0;
    while (*s && *s != '*') csum ^= (uint8_t)*s++;
    if (*s != '*') return false;
    return csum == (uint8_t)strtol(s + 1, nullptr, 16);
}

#define GPS_EXT_MIN_SATS     6   // Mindest-Satelliten für gültigen Fix
#define GPS_EXT_SETTLE_MS 20000  // 20s Einschwingzeit nach Boot ignorieren

static bool    s_rtc_synced   = false;
static bool    s_pos_settled  = false;  // Position nach Boot stabil?
static uint32_t s_first_fix_ms = 0;

static void parse_gnrmc(char* buf) {
    if (nmea_field(buf, 2)[0] != 'A') { gps_invalidate(); return; }
    double lat = nmea_deg(nmea_field(buf, 3), nmea_field(buf, 4)[0]);
    double lon = nmea_deg(nmea_field(buf, 5), nmea_field(buf, 6)[0]);
    float  spd = (float)atof(nmea_field(buf, 7)) * 1.852f;
    float  crs = (float)atof(nmea_field(buf, 8));
    if (lat == 0.0 && lon == 0.0) { gps_invalidate(); return; }

    // Qualitäts-Gate: Mindest-Sats + Einschwingzeit nach Boot
    if (s_sat_count < GPS_EXT_MIN_SATS) { gps_invalidate(); return; }
    if (!s_pos_settled) {
        if (s_first_fix_ms == 0) s_first_fix_ms = millis();
        if ((millis() - s_first_fix_ms) < GPS_EXT_SETTLE_MS) { gps_invalidate(); return; }
        s_pos_settled = true;
        syslog("GPS_EXT", "Position stabil — Tracking aktiv");
    }

    gps_update(lat, lon, spd, crs);

    // ── RTC einmalig aus GPS-UTC-Zeit setzen ──────────────────
    if (!s_rtc_synced) {
        // Feld 1: HHMMSS.ss  Feld 9: DDMMYY
        const char* tstr = nmea_field(buf, 1);
        const char* dstr = nmea_field(buf, 9);
        if (tstr[0] && dstr[0] && dstr[0] != ',') {
            int hh = (tstr[0]-'0')*10 + (tstr[1]-'0');
            int mi = (tstr[2]-'0')*10 + (tstr[3]-'0');
            int ss = (tstr[4]-'0')*10 + (tstr[5]-'0');
            int dd = (dstr[0]-'0')*10 + (dstr[1]-'0');
            int mo = (dstr[2]-'0')*10 + (dstr[3]-'0');
            int yy = (dstr[4]-'0')*10 + (dstr[5]-'0');
            if (hh < 24 && mi < 60 && ss < 60 && dd >= 1 && dd <= 31 && mo >= 1 && mo <= 12) {
                rtc_set_datetime(2000 + yy, mo, dd, hh, mi, ss);
                s_rtc_synced = true;
                char msg[48];
                snprintf(msg, sizeof(msg), "RTC sync via GPS: %04d-%02d-%02d %02d:%02d:%02d",
                         2000+yy, mo, dd, hh, mi, ss);
                syslog("GPS_EXT", msg);
            }
        }
    }
}

static void parse_gngga(char* buf) {
    if (atoi(nmea_field(buf, 6)) > 0)
        s_sat_count = atoi(nmea_field(buf, 7));
}

// ── GSV: sichtbare Satelliten zählen ─────────────────────
// Nicht über Feld 3 ("SVs in view") summieren: ab NMEA 4.11 sendet ein
// Mehrband-Empfänger je Signal einen eigenen GSV-Satz, derselbe Satellit
// taucht also auf L1 und L5 getrennt auf. Aufsummiert ergäbe das die
// doppelte Anzahl. Deshalb die SV-IDs sammeln und eindeutige zählen.
#define GSV_MAX_SV 64
static int      s_sats_visible = 0;
static uint16_t s_gsv_ids[GSV_MAX_SV];
static uint8_t  s_gsv_n = 0;

static void gsv_add(uint8_t talker_c, int svid) {
    if (svid <= 0) return;
    // Talker-Kennbuchstabe (P/L/A/B/Q/I) trennt gleiche SV-Nummern
    // verschiedener Konstellationen.
    uint16_t key = ((uint16_t)talker_c << 8) | (uint8_t)svid;
    for (uint8_t i = 0; i < s_gsv_n; i++) if (s_gsv_ids[i] == key) return;
    if (s_gsv_n < GSV_MAX_SV) s_gsv_ids[s_gsv_n++] = key;
}

static void parse_gsv(char* buf, const char* talker) {
    // Aufbau: $xxGSV,numMsg,msgNum,numSV, {svid,elev,azim,cno} ×4 [,signalId]
    // Die SV-IDs stehen damit auf Feld 4, 8, 12 und 16.
    for (int f = 4; f <= 16; f += 4) {
        const char* s = nmea_field(buf, f);
        if (!s || s[0] == '\0' || s[0] == ',' || s[0] == '*') break;
        gsv_add((uint8_t)talker[1], atoi(s));
    }
}

// Zyklus abschließen — wird beim Beginn einer neuen Epoche (RMC) gerufen.
static void gsv_cycle_end() {
    if (s_gsv_n > 0) s_sats_visible = s_gsv_n;
    s_gsv_n = 0;
}

static bool s_nmea_valid = false;  // erstes Paket mit gültiger Checksumme empfangen

static void process_line(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
    if (len < 6 || !nmea_checksum_ok(line)) return;
    // Erst nach validem NMEA gilt Modul als bereit
    if (!s_nmea_valid) {
        s_nmea_valid = true;
        s_ok = true;  // gps_ext_ok() erst jetzt true
        syslog("GPS_EXT", "NMEA-Daten empfangen — Modul antwortet");
        inject_time();  // Immer — beschleunigt Fix von ~60s auf ~22s
    }
    if      (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
        // RMC eröffnet die Epoche — davor gesammelte GSV-Sätze sind vollständig.
        // Unabhängig davon, wie viele Signal-Durchläufe das Modul gesendet hat.
        gsv_cycle_end();
        parse_gnrmc(line);
    }
    else if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) parse_gngga(line);
    else if (strncmp(line+3, "GSV", 3) == 0) parse_gsv(line, line + 1);
}

// ── FreeRTOS Task ─────────────────────────────────────────
static void gps_ext_task(void*) {
    static char    line[128];
    static uint8_t pos = 0;
    static bool    fix_logged  = false;   // erster Fix geloggt?
    static uint32_t last_status_ms = 0;
    static int      last_sat   = -1;

    while (!g_shutdown) {
        while (s_gps_uart.available()) {
            char c = (char)s_gps_uart.read();
            if (c == '$') pos = 0;
            if (pos < sizeof(line) - 1) {
                line[pos++] = c;
                if (c == '\n') { line[pos] = '\0'; process_line(line); pos = 0; }
            } else { pos = 0; }
        }

        uint32_t now = millis();

        // Kein NMEA nach 5s → warnen
        if (!s_nmea_valid && now > 5000 && (now - last_status_ms) > 5000) {
            last_status_ms = now;
            syslog("GPS_EXT", "WARNUNG: Keine NMEA-Daten — Verdrahtung prüfen");
        }

        // Status alle 30s (immer loggen solange kein Fix, danach nur bei Änderung)
        if (s_nmea_valid && (now - last_status_ms) >= 30000UL) {
            last_status_ms = now;
            bool has_fix = gps_valid();
            if (s_sat_count != last_sat || !has_fix) {
                last_sat = s_sat_count;
                char msg[64];
                snprintf(msg, sizeof(msg), "Sats: %d sichtbar / %d im Fix · %s",
                         s_sats_visible, s_sat_count, has_fix ? "Fix" : "suche...");
                syslog("GPS_EXT", msg);
            }
        }

        // Erster Fix
        if (!fix_logged && gps_valid()) {
            fix_logged = true;
            char msg[64];
            snprintf(msg, sizeof(msg), "Fix! Sats: %d sichtbar / %d im Fix · %.5f / %.5f",
                     s_sats_visible, s_sat_count, gps_lat(), gps_lon());
            syslog("GPS_EXT", msg);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

// ── Baudraten-Erkennung ──────────────────────────────────
// Lauscht, bis eine NMEA-Zeile mit gültiger Prüfsumme hereinkommt.
static bool nmea_sniff(uint32_t timeout_ms) {
    char     line[128];
    uint8_t  pos   = 0;
    uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        while (s_gps_uart.available()) {
            char c = (char)s_gps_uart.read();
            if (c == '$') pos = 0;
            if (pos >= sizeof(line) - 1) { pos = 0; continue; }
            line[pos++] = c;
            if (c != '\n') continue;
            line[pos] = '\0';
            pos = 0;
            size_t l = strlen(line);
            while (l > 0 && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = '\0';
            if (l >= 6 && line[0] == '$' && nmea_checksum_ok(line)) return true;
        }
        delay(5);
    }
    return false;
}

// Probiert die gängigen u-blox-Raten durch. Das trennt die beiden Fehlerbilder,
// die im Log sonst identisch aussehen: liefert KEINE Rate gültiges NMEA, liegt
// es an Verdrahtung oder Versorgung — und nicht an der Baudrate.
static bool detect_baud() {
    static const uint32_t cand[] = { GPS_EXT_BAUD, 38400, 115200, 9600 };
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        bool seen = false;
        for (size_t j = 0; j < i; j++) if (cand[j] == cand[i]) seen = true;
        if (seen) continue;                       // Startwert kann doppelt vorkommen

        s_gps_uart.end();
        s_gps_uart.begin(cand[i], SERIAL_8N1, GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);
        delay(50);
        char m[64];
        if (nmea_sniff(1200)) {
            s_baud = cand[i];
            snprintf(m, sizeof(m), "Baudrate erkannt: %lu Bd", (unsigned long)s_baud);
            syslog("GPS_EXT", m);
            return true;
        }
        snprintf(m, sizeof(m), "%lu Bd: kein NMEA", (unsigned long)cand[i]);
        syslog("GPS_EXT", m);
    }
    s_baud = 0;
    s_gps_uart.end();
    s_gps_uart.begin(GPS_EXT_BAUD, SERIAL_8N1, GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);
    syslog("GPS_EXT", "Keine Baudrate liefert NMEA — Verdrahtung/Versorgung pruefen");
    return false;
}

// ── Öffentliche API ───────────────────────────────────────
// ── Frühes Wake: UART öffnen — sofort nach pmu_init() aufrufen ──
void gps_ext_wake() {
    if (!GPS_EXT_ENABLED) return;
    // Nach Deep Sleep steht die erkannte Rate noch im RTC-Speicher.
    s_gps_uart.begin(s_baud ? s_baud : (uint32_t)GPS_EXT_BAUD, SERIAL_8N1,
                     GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);
}

void gps_ext_init() {
    if (!GPS_EXT_ENABLED) return;
    if (!s_gps_uart) {
        gps_ext_wake();
    }
    delay(100);

    // Gemerkte Rate erst gegenprüfen, sonst von vorn suchen.
    bool answering = s_baud ? nmea_sniff(1200) : false;
    if (s_baud && !answering) {
        syslog("GPS_EXT", "Gemerkte Baudrate liefert nichts — suche neu");
    }
    if (!answering) answering = detect_baud();

    // Konfigurieren nur wenn das Modul antwortet — und nur bei Cold Start
    if (answering && !sleep_was_deep()) {
        enable_active_antenna();
        enable_all_gnss();
        enable_aop();
    }
    // PPS-LED wieder an (war im Sleep deaktiviert)
    if (answering) enable_timepulse();

    // s_ok wird erst nach erstem validen NMEA-Paket gesetzt (in process_line)
    { char m[96]; snprintf(m, sizeof(m), "Ext. GPS — UART GPIO%d/%d %luBd (warte auf Fix)",
                           GPS_EXT_RX_PIN, GPS_EXT_TX_PIN,
                           (unsigned long)(s_baud ? s_baud : (uint32_t)GPS_EXT_BAUD));
      syslog("GPS_EXT", m); }
    xTaskCreatePinnedToCore(gps_ext_task, "GPS_EXT", 4096, NULL, 2, NULL, 1);
}

// ── GPS in Idle schicken — M10 trackt weiter, UART wird geschlossen ──
// Kein Backup-Befehl! M10 verliert bei Backup seine Nav-Daten auf diesem Modul.
// Stattdessen: M10 läuft weiter (~5-8mA), behält Fix + Ephemeris + AOP.
// Nach Wake: UART öffnen → sofortiger Fix.
void gps_ext_sleep() {
    if (!GPS_EXT_ENABLED) return;
    // PPS-LED aus (spart etwas Strom, kosmetisch)
    disable_timepulse();
    s_gps_uart.flush();
    s_gps_uart.end();
    syslog("GPS_EXT", "Idle-Sleep (M10 trackt weiter, UART closed)");
}

bool gps_ext_ok()        { return s_ok; }
int  gps_ext_sat_count()   { return s_sat_count; }
int  gps_ext_sat_visible() { return s_sats_visible; }
