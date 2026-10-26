#include "mod_gps_ext.h"
#include "mod_logs.h"
#include "mod_sleep.h"
#include "mod_rtc.h"
#include "mod_pmu.h"     // pmu_set_ext_power() für den Powercycle-Test
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

// Handle des Lese-Tasks. Wird nur für die Bench-Tests gebraucht: der Task liest
// laufend vom selben UART, und eine parallel laufende Messung bekäme dadurch
// mitten aus ihrer UBX-Antwort Bytes entwendet — Kopf noch passend, Nutzdaten
// verschoben. Das ergibt Werte wie "137 Satelliten" und ist als Messung wertlos.
// Deshalb wird der Task während einer Messung angehalten.
static TaskHandle_t s_task = nullptr;
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

static uint8_t s_ubx_buf[768];   // gemeinsamer Empfangspuffer, nicht auf den Stack

// ── Auf eine bestimmte UBX-Nachricht warten ──────────────
// Liefert die Nutzdatenlänge oder -1. Wie beim ACK läuft ein Schiebefenster
// über den Bytestrom, damit zwischendurch einlaufende NMEA-Zeilen nicht stören.
static int ubx_recv(uint8_t cls, uint8_t id, uint8_t* out, uint16_t out_size,
                    uint32_t timeout_ms) {
    uint8_t  hdr[6] = {};
    uint32_t start  = millis();
    while ((millis() - start) < timeout_ms) {
        if (!s_gps_uart.available()) { delay(1); continue; }
        memmove(hdr, hdr + 1, 5);
        hdr[5] = (uint8_t)s_gps_uart.read();
        if (hdr[0] != 0xB5 || hdr[1] != 0x62 ||
            hdr[2] != cls  || hdr[3] != id) continue;

        uint16_t len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
        if (len > out_size) return -1;              // passt nicht → verwerfen
        uint16_t got = 0;
        while (got < len && (millis() - start) < timeout_ms + 1000) {
            if (s_gps_uart.available()) out[got++] = (uint8_t)s_gps_uart.read();
            else delay(1);
        }
        return (got == len) ? (int)len : -1;
    }
    return -1;
}

// ── UBX-MON-VER abfragen — welches Modul, welche Firmware? ───
// Ohne diese Angabe ist jede Aussage über unterstützte Konfigurationsschlüssel
// geraten: M10 und F10 teilen zwar das VALSET-Interface, aber nicht die
// Schlüsselmenge. Antwortlayout: swVersion[30] + hwVersion[10] + N×30 Byte
// Erweiterungen, in denen MOD=<Modulname> und PROTVER=<x.yy> stehen.
// MON-VER gibt es in jeder u-blox-Generation, der Poll ist also risikofrei.
static void log_mon_ver() {
    while (s_gps_uart.available()) s_gps_uart.read();
    ubx_send(0x0A, 0x04, nullptr, 0);

    int len = ubx_recv(0x0A, 0x04, s_ubx_buf, sizeof(s_ubx_buf), 1500);
    if (len < 40) { syslog("GPS_EXT", "MON-VER: keine Antwort"); return; }

    char sw[31] = {}, hw[11] = {}, m[128];
    memcpy(sw, s_ubx_buf, 30);
    memcpy(hw, s_ubx_buf + 30, 10);
    snprintf(m, sizeof(m), "Modul: HW %s · SW %s", hw, sw);
    syslog("GPS_EXT", m);

    // Erweiterungen: je 30 Byte, u.a. MOD=<Modulname> und PROTVER=<x.yy>
    for (int off = 40; off + 30 <= len; off += 30) {
        char ext[31] = {};
        memcpy(ext, s_ubx_buf + off, 30);
        if (ext[0] == '\0') continue;
        snprintf(m, sizeof(m), "Modul: %s", ext);
        syslog("GPS_EXT", m);
    }
}

// ── Wie breit ist der Wert zu einem Konfigurationsschlüssel? ──
// Schlüsselformat: Bit 30..28 = Größenkennung, Bit 27..16 = Gruppe,
// Bit 15..0 = Item. Ohne die Größe lässt sich die Antwort nicht zerlegen.
static uint8_t cfg_val_size(uint32_t key) {
    switch ((key >> 28) & 0x07) {
        case 1: return 1;   // L  (1 Bit, überträgt 1 Byte)
        case 2: return 1;   // U1 / I1 / E1
        case 3: return 2;   // U2 / I2 / E2
        case 4: return 4;   // U4 / I4 / E4 / R4
        case 5: return 8;   // U8 / I8 / R8
        default: return 0;  // unbekannt → Zerlegen abbrechen
    }
}

// ── CFG-VALGET: alle Schlüssel einer Gruppe auflisten ────
// Laut Interface Description ist ein Schlüssel mit 0xFFFF im Item-Teil eine
// Sammelabfrage über die gesamte Gruppe. Damit sagt das Modul selbst, welche
// Signale es kennt und wie sie gerade stehen — statt dass wir Hex-IDs aus
// Datenblättern raten und uns dafür NAKs einfangen. Reines Lesen, kein Schreiben.
static void dump_cfg_group(const char* what, uint16_t group) {
    while (s_gps_uart.available()) s_gps_uart.read();

    uint32_t wildcard = ((uint32_t)(group & 0x0FFF) << 16) | 0xFFFFu;
    uint8_t  req[8];
    req[0] = 0x00;                 // version 0 = Anfrage
    req[1] = 0x00;                 // layer 0 = RAM (der gerade wirksame Stand)
    req[2] = 0x00; req[3] = 0x00;  // position (Blättern, hier nicht nötig)
    req[4] = (uint8_t)(wildcard);
    req[5] = (uint8_t)(wildcard >> 8);
    req[6] = (uint8_t)(wildcard >> 16);
    req[7] = (uint8_t)(wildcard >> 24);
    ubx_send(0x06, 0x8B, req, 8);

    char m[128];
    int len = ubx_recv(0x06, 0x8B, s_ubx_buf, sizeof(s_ubx_buf), 2000);
    if (len < 4) {
        snprintf(m, sizeof(m), "%s: keine Antwort auf VALGET-Sammelabfrage", what);
        syslog("GPS_EXT", m);
        return;
    }

    int count = 0;
    for (int off = 4; off + 4 <= len; ) {          // ab 4: version, layer, position
        uint32_t key = (uint32_t)s_ubx_buf[off]
                     | ((uint32_t)s_ubx_buf[off + 1] << 8)
                     | ((uint32_t)s_ubx_buf[off + 2] << 16)
                     | ((uint32_t)s_ubx_buf[off + 3] << 24);
        uint8_t vs = cfg_val_size(key);
        if (vs == 0 || off + 4 + vs > len) break;  // unlesbar → hier abbrechen

        uint64_t val = 0;
        for (uint8_t i = 0; i < vs; i++) val |= (uint64_t)s_ubx_buf[off + 4 + i] << (8 * i);
        snprintf(m, sizeof(m), "%s: 0x%08lX = %lu",
                 what, (unsigned long)key, (unsigned long)val);
        syslog("GPS_EXT", m);

        off += 4 + vs;
        count++;
    }
    snprintf(m, sizeof(m), "%s: %d Schluessel gemeldet", what, count);
    syslog("GPS_EXT", m);
}

// ── Hat der Backup-Bereich getragen? ─────────────────────
// UBX-NAV-PVT, Offset 11 = valid: Bit0 validDate, Bit1 validTime. Sind beide
// gesetzt, lief die RTC des Moduls durch — dann steht auch BBR mit Ephemeriden
// und die erste Position ist sofort brauchbar. Ist der Backup-Bereich dagegen
// leer, fängt das Modul bei null an und die ersten Fixes wandern noch.
// Genau daran hängt, ob die Einschwingsperre nötig ist oder nur Zeit kostet.
static bool module_time_valid() {
    for (uint8_t t = 0; t < 3; t++) {
        while (s_gps_uart.available()) s_gps_uart.read();
        ubx_send(0x01, 0x07, nullptr, 0);
        int len = ubx_recv(0x01, 0x07, s_ubx_buf, sizeof(s_ubx_buf), 1200);
        if (len >= 12) return (s_ubx_buf[11] & 0x03) == 0x03;
        delay(200);
    }
    return false;   // keine Antwort → sicherheitshalber wie Kaltstart behandeln
}

// ── Letzte bekannte Position ─────────────────────────────
// Beim Kaltstart weiß das Modul nicht, wo es steht, und muss den ganzen Himmel
// absuchen. Ein grober Startpunkt verkürzt das erheblich. Die Position landet
// in NVS, überlebt also auch den völligen Stromausfall — anders als RTC_DATA_ATTR.
// Geschrieben wird nur beim Schlafenlegen und höchstens alle 10 Minuten, damit
// der Flash nicht unnötig altert.
static uint32_t s_pos_saved_ms = 0;

static void save_last_pos(bool force) {
    if (!gps_valid()) return;
    uint32_t now = millis();
    if (!force && s_pos_saved_ms && (now - s_pos_saved_ms) < 600000UL) return;
    Preferences p;
    if (!p.begin("gpsext", false)) return;
    p.putDouble("lat", gps_lat());
    p.putDouble("lon", gps_lon());
    p.end();
    s_pos_saved_ms = now;
}

// UBX-MGA-INI-POS_LLH: Typ 0x01, 20 Byte. lat/lon in 1e-7 Grad, alt in cm,
// posAcc in cm. Die Genauigkeitsangabe ist bewusst grob — sie sagt dem Modul
// nur, wie weit es suchen soll, und darf nicht zu optimistisch sein.
static void inject_pos() {
    Preferences p;
    if (!p.begin("gpsext", true)) return;
    double lat = p.getDouble("lat", 0.0);
    double lon = p.getDouble("lon", 0.0);
    p.end();
    if (lat == 0.0 && lon == 0.0) return;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;

    int32_t  la = (int32_t)(lat * 1e7);
    int32_t  lo = (int32_t)(lon * 1e7);
    uint32_t acc = 100000;             // 1 km — reicht als Suchraum, bleibt ehrlich

    uint8_t q[20] = {};
    q[0] = 0x01;                       // type = POS_LLH
    q[1] = 0x00;                       // version
    memcpy(q + 4,  &la,  4);
    memcpy(q + 8,  &lo,  4);
    // q[12..15] alt = 0 cm — Höhe kennen wir nicht, 0 ist als Startwert brauchbar
    memcpy(q + 16, &acc, 4);
    ubx_send(0x13, 0x40, q, 20);

    char m[80];
    snprintf(m, sizeof(m), "Startposition injiziert: %.4f / %.4f", lat, lon);
    syslog("GPS_EXT", m);
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


// ── Antennenspannung: bewusst KEIN Konfigurationsbefehl ──
// Früher stand hier ein VALSET auf CFG-HW-ANT_CFG_VOLTCTRL (0x10A3002E). Das
// Modul hat es zuverlässig mit NAK quittiert. Der Schlüssel fehlt der Firmware
// nicht — er steht in der F10-SPG-Schlüsselliste, Default 0. Ein VALSET auf den
// RAM-Layer löst aber eine Gültigkeitsprüfung aus, und die schlägt hier fehl:
// der Antennen-Supervisor braucht die PIOs ANT0/ANT1/ANT2, und der DAN-F10N
// führt sie nicht nach außen (MAX-F10S tut es, daher steht der Schlüssel in der
// gemeinsamen Firmware). Beim DAN-F10N ist die Antennenwahl Hardware:
// Pin 32 ANT_CTRL wählt interne Patch- oder externe Antenne, die Speisung einer
// externen aktiven Antenne läuft über VCC_RF (Pin 31) per Bias-Tee, geschaltet
// von LNA_EN (Pin 39). Per Software ist da nichts zu holen.

// ── Einen einzelnen Konfigurationswert lesen (CFG-VALGET) ──
// true = Modul kennt den Schlüssel und hat geantwortet.
// Während eines GNSS-Neustarts bleibt das Modul kurz stumm, deshalb mehrere
// Versuche — ein einzelner Fehlschlag heißt hier nicht "Schlüssel fehlt".
static bool cfg_read_u1(uint32_t key, uint8_t& out, uint8_t layer = 0,
                        uint8_t tries = 3) {
    for (uint8_t t = 0; t < tries; t++) {
        while (s_gps_uart.available()) s_gps_uart.read();
        uint8_t req[8];
        req[0] = 0x00;                 // version 0 = Anfrage
        req[1] = layer;                // 0=RAM (wirksamer Stand), 1=BBR, 2=Flash
        req[2] = 0x00; req[3] = 0x00;  // position
        req[4] = (uint8_t)(key);
        req[5] = (uint8_t)(key >> 8);
        req[6] = (uint8_t)(key >> 16);
        req[7] = (uint8_t)(key >> 24);
        ubx_send(0x06, 0x8B, req, 8);

        int len = ubx_recv(0x06, 0x8B, s_ubx_buf, sizeof(s_ubx_buf), 1200);
        if (len >= 9) {
            uint32_t got = (uint32_t)s_ubx_buf[4]
                         | ((uint32_t)s_ubx_buf[5] << 8)
                         | ((uint32_t)s_ubx_buf[6] << 16)
                         | ((uint32_t)s_ubx_buf[7] << 24);
            if (got == key) { out = s_ubx_buf[8]; return true; }
        }
        delay(200);
    }
    return false;
}

// ── Schlüssel auf den gewünschten Wert bringen ───────────
// Zwei Eigenheiten des F10, beide am Gerät gemessen:
//  1. Jedes CFG-SIGNAL-VALSET startet das GNSS-Subsystem neu. Steht der Wert
//     schon richtig, wird deshalb gar nicht geschrieben — das spart bei jedem
//     Start mehrere Neustarts und damit Zeit bis zum ersten Fix. Da RAM+BBR
//     dank Stützbatterie erhalten bleiben, ist das der Normalfall.
//  2. Während dieses Neustarts geht das ACK gelegentlich verloren, obwohl der
//     Wert übernommen wurde. Maßgeblich ist deshalb das Rücklesen, nicht das
//     ACK — sonst meldet das Log Fehlschläge, die keine sind.
// read_layer: welche Ebene beim Prüfen gelesen wird. Für einen BBR-Schreibzugriff
// muss die BBR-Ebene gelesen werden — die RAM-Ebene steht dann längst richtig und
// der Schreibzugriff unterbliebe sonst.
static bool ensure_key(const char* name, uint32_t key, uint8_t want,
                       uint8_t layers, uint16_t settle_ms, uint8_t read_layer = 0) {
    char    m[112];
    uint8_t cur = 0;

    if (cfg_read_u1(key, cur, read_layer) && cur == want) {
        snprintf(m, sizeof(m), "%s: steht bereits auf %u", name, want);
        syslog("GPS_EXT", m);
        return true;
    }

    while (s_gps_uart.available()) s_gps_uart.read();
    uint8_t p[9] = {};
    p[0] = 0x00;              // version
    p[1] = layers;            // Bit0=RAM, Bit1=BBR, Bit2=Flash
    p[2] = 0x00; p[3] = 0x00; // reserved
    p[4] = (uint8_t)(key);
    p[5] = (uint8_t)(key >> 8);
    p[6] = (uint8_t)(key >> 16);
    p[7] = (uint8_t)(key >> 24);
    p[8] = want;
    ubx_send(0x06, 0x8A, p, 9);
    int ack = ubx_wait_ack(0x06, 0x8A, 3000);

    delay(settle_ms);                       // u-blox: ACK abwarten, dann 0,5 s
    bool verified = cfg_read_u1(key, cur, read_layer) && cur == want;

    snprintf(m, sizeof(m), "%s: %s", name,
             verified ? (ack == 1 ? "gesetzt (ACK)" : "gesetzt (per Ruecklesen bestaetigt)")
             : ack == 0 ? "NAK — vom Modul abgelehnt"
                        : "fehlgeschlagen (kein ACK, Ruecklesen negativ)");
    syslog("GPS_EXT", m);
    return verified;
}

// ── AssistNow Autonomous (AOP) aktivieren ────────────────
// CFG-ANA-USE_ANA (0x10230001), auf F10 SPG 6.00 bestätigt, Default 0.
// Modul sammelt Satellitendaten und erstellt eigene Orbit-Vorhersagen.
// RAM+BBR, damit es die Stützbatterie überlebt und nicht bei jedem Start
// neu erarbeitet werden muss. Kein CFG-SIGNAL → kein GNSS-Neustart.
static void enable_aop() {
    ensure_key("AssistNow Autonomous", 0x10230001UL, 1, 0x03, 100);
}

// ── GNSS-Konstellationen für den DAN-F10N ───────────────
// Am Gerät verifiziert — MON-VER meldet MOD=DAN-F10N, FWVER=SPGL1L5 6.00,
// PROTVER=40.00, und das Modul nennt seine Konstellationen selbst:
// GPS;GAL;BDS + SBAS;QZSS + NAVIC.
//
// Bewusst NICHT konfiguriert:
//   GLONASS — CFG-SIGNAL-GLO_ENA (0x10310025) gibt es auf F10 SPG nicht.
//             Direkt am Modul geprüft: VALGET liefert keine Antwort, VALSET
//             quittiert mit NAK. Der F10 ist ein L1+L5-Empfänger, GLONASS L1OF
//             sendet FDMA um 1602 MHz und liegt außerhalb des L1-Filters.
//             Kein Firmware-Update ändert daran etwas.
//   QZSS / NavIC — vom Modul unterstützt (0x10310024 / 0x10310026, beide ACK),
//             aber Regionalsysteme über Ostasien bzw. Indien. Auf mitteleuropäi-
//             scher Breite ist davon kein Satellit sichtbar; einschalten kostet
//             nur GNSS-Neustarts. Wer das Gerät dorthin mitnimmt, setzt die
//             beiden Schlüssel auf 1 — mehr ist nicht nötig.
//
// GPS/Galileo/BeiDou/SBAS stehen ab Werk bereits auf 1; ensure_key() liest das
// und schreibt dann gar nicht erst.
static void configure_gnss() {
    static const struct { const char* name; uint32_t key; } konst[] = {
        { "GPS",     0x1031001FUL },  // CFG-SIGNAL-GPS_ENA
        { "Galileo", 0x10310021UL },  // CFG-SIGNAL-GAL_ENA
        { "BeiDou",  0x10310022UL },  // CFG-SIGNAL-BDS_ENA
        { "SBAS",    0x10310020UL },  // CFG-SIGNAL-SBAS_ENA — EGNOS über Europa
    };
    char ok[88] = {};
    bool all = true;
    for (size_t i = 0; i < sizeof(konst) / sizeof(konst[0]); i++) {
        // RAM+BBR: überlebt dank Stützbatterie den Sleep, danach keine Neustarts mehr.
        if (ensure_key(konst[i].name, konst[i].key, 1, 0x03, 600)) {
            if (ok[0]) strncat(ok, "+", sizeof(ok) - strlen(ok) - 1);
            strncat(ok, konst[i].name, sizeof(ok) - strlen(ok) - 1);
        } else {
            all = false;
        }
    }
    char m[112];
    snprintf(m, sizeof(m), "GNSS aktiv: %s", ok[0] ? ok : "(keine bestaetigt!)");
    syslog("GPS_EXT", m);
    if (!all) dump_cfg_group("SIG", 0x31);   // nur im Fehlerfall die ganze Gruppe zeigen
}

// ── GPS-L5-Health-Override ───────────────────────────────
// GPS L5 ist offiziell noch pre-operational: die Satelliten senden sich selbst
// als "unhealthy". Das Modul empfängt sie sauber, wirft sie aber aus der
// Positionslösung. Am Gerät gemessen (UBX-NAV-SIG):
//     ohne Override:  GPS L5 getrackt=5, in Loesung=0, als krank=5
//     mit  Override:  GPS L5 getrackt=5, in Loesung=5, als krank=0
//     Signale in der Loesung gesamt: 32 → 37
// Mit diesem Schlüssel übernimmt das Modul für L5 den Health-Status desselben
// Satelliten aus L1 C/A. Galileo E5a und BeiDou B2a brauchen das nicht, die
// senden gesund und gingen schon vorher in die Lösung ein.
//
// Der Schlüssel liegt in Gruppe 0x32, NICHT in CFG-SIGNAL (0x31). In der F10-SPG
// Interface Description fehlt er — dort gibt es überhaupt keine Gruppe 0x32.
// Dokumentiert ist er im DAN-F10N Integration Manual §2.1.3 (Tabellen 3 und 4),
// dort allerdings nur als fertige Bytefolge statt als benannter Schlüssel, und
// zwar ausdrücklich für RAM, BBR und Flash. Eine Sammelabfrage auf Gruppe 0x32
// bleibt unbeantwortet, einzeln lässt er sich lesen.
//
// Deshalb zwei Schreibzugriffe statt einem:
//   RAM — wirkt sofort ohne Neustart, wird aber vom Software-Standby gelöscht
//         ("Entering the software standby mode clears the RAM memory including
//         the receiver configuration", Integration Manual §3.5.2.2).
//   BBR — überlebt den Standby, wirkt laut Handbuch aber erst nach einem
//         Neustart. Als alleinige Ebene also zu spät, als Ergänzung richtig.
// Ob das Aufwachen aus dem Standby schon als Neustart zählt, sagt u-blox nicht.
// Am Gerät nachgemessen ("gps standby"): ja — nach dem Aufwachen steht der
// Schlüssel in beiden Ebenen wieder auf 1, die RAM-Ebene wird also aus BBR neu
// aufgebaut. Das erneute Prüfen in gps_ext_init() ist damit die zweite
// Sicherung und nicht die einzige.
static void enable_gps_l5() {
    ensure_key("GPS-L5-Override (RAM)", 0x10320001UL, 1, 0x01, 600, /*read=*/0);
    ensure_key("GPS-L5-Override (BBR)", 0x10320001UL, 1, 0x02, 200, /*read=*/1);
}

// ── TIMEPULSE ein/aus (PPS-LED) ──────────────────────────
// Nur RAM — Modul läuft durch, kein BBR nötig
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
#define GPS_EXT_SETTLE_MS 20000  // Einschwingzeit — nur nach echtem Kaltstart

static bool    s_rtc_synced   = false;
static bool    s_pos_settled  = false;  // Position nach Boot stabil?
static uint32_t s_first_fix_ms = 0;

// Verfallszeit des Positions-Caches. Ohne die bleibt g_gps.valid für immer true,
// sobald das Modul verstummt — bei einem Rail-Glitch auf DC5, einer gestörten
// UART-Leitung oder einem Modul-Hänger. gps_invalidate() wird nämlich nur aus
// process_line() gerufen, also nur wenn Daten ANKOMMEN. Am Gerät nachgewiesen
// ("gps powercycle 30"): 30 s ohne jede Versorgung, und der Cache meldete
// durchgehend eine gültige Position samt Geschwindigkeit. Genau diese Werte
// gehen in den Telemetriepuffer und über MQTT hinaus.
//
// 10 s ist bewusst großzügig gewählt. Das Modul liefert RMC im Sekundentakt,
// zehn ausgelassene Epochen sind also kein normaler Empfangsausfall mehr,
// sondern ein Fehler. Kürzer zu greifen wäre riskant: der GPS-Task kann unter
// Last (CAN-Scan, Log-Download über den Webserver) durchaus einige Sekunden
// verzögert werden, und eine voreilige Verwerfung kostete echte Messpunkte.
// Der Zustand heilt sich selbst — die nächste gültige Position setzt ihn zurück.
#define GPS_EXT_STALE_MS 10000
static uint32_t s_last_pos_ms = 0;

// Wie lange die erste Position verworfen wird. Früher fest 20 s, unabhängig
// vom Zustand des Moduls — das kostete auch dann 20 Sekunden, wenn das Modul
// mit intaktem Backup hochkam und seine Position von Anfang an stimmte.
// gps_ext_init() setzt den Wert: 0 bei Warmstart, GPS_EXT_SETTLE_MS bei
// Kaltstart, wo die ersten Fixes tatsächlich noch wandern.
static uint32_t s_settle_ms = GPS_EXT_SETTLE_MS;

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
        if ((millis() - s_first_fix_ms) < s_settle_ms) { gps_invalidate(); return; }
        s_pos_settled = true;
        char m[64];
        snprintf(m, sizeof(m), "Position stabil — Tracking aktiv (Sperre %lus)",
                 (unsigned long)(s_settle_ms / 1000));
        syslog("GPS_EXT", m);
    }

    gps_update(lat, lon, spd, crs);
    s_last_pos_ms = millis();   // Altersstempel für die Verfallsprüfung im Task
    save_last_pos(false);   // gelegentlich sichern — hilft dem nächsten Kaltstart

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

// Wie viele Satelliten trägt DIESER GSV-Satz? 0 = unbrauchbar.
// Aufbau: $xxGSV,numMsg,msgNum,numSV,{svid,elev,azim,cno} ×n[,signalId]
// Blind vier Blöcke zu lesen greift beim letzten Satz einer Serie daneben: dort
// steht auf der freien Position die signalId, und die ist bei L5/E5a/B2a
// ungleich null. Am Gerät nachgewiesen ("gps gsv"):
//     $GAGSV,2,2,06,23,18,039,21,31,52,086,26,7*7B   → gelesen: 23, 31, und 7
//     $GBGSV,2,2,07,22,...,37,53,068,30,3*4B         → gelesen: 22, 23, 37, und 3
// Die 7 und die 3 sind Signal-IDs. So entstanden je Konstellation und Signal
// Phantom-Satelliten; gemeldet wurden 42 statt tatsächlich rund 38.
//
// numSV und msgNum sagen es exakt und kommen ohne Annahme über die Zahl der
// Felder aus — insbesondere ohne die, dass ein leeres C/N0-Feld das Satzende
// markiert. Leere C/N0 sind normal für Satelliten, die sichtbar aber nicht
// getrackt sind.
static int gsv_sats_in_msg(char* buf) {
    int msg_nr = atoi(nmea_field(buf, 2));
    int num_sv = atoi(nmea_field(buf, 3));
    if (msg_nr < 1 || num_sv < 1) return 0;
    int n = num_sv - (msg_nr - 1) * 4;
    if (n > 4) n = 4;
    return (n < 1) ? 0 : n;
}

static void parse_gsv(char* buf, const char* talker) {
    int n = gsv_sats_in_msg(buf);
    for (int i = 0; i < n; i++) {
        const char* s = nmea_field(buf, 4 + i * 4);
        if (!s || s[0] == '\0' || s[0] == ',' || s[0] == '*') break;
        gsv_add((uint8_t)talker[1], atoi(s));
    }
}

// Zyklus abschließen — wird beim Beginn einer neuen Epoche (RMC) gerufen.
static bool s_gsv_first_cycle = true;
static void gsv_cycle_end() {
    // Der allererste Zyklus ist zwangsläufig unvollständig: beim Start hängt sich
    // der Parser mitten in einen laufenden GSV-Durchlauf. Übernommen ergäbe das
    // Meldungen wie "1 sichtbar / 12 im Fix", die sich eine Epoche später selbst
    // widerlegen. Also verwerfen und erst ab dem zweiten Zyklus zählen.
    if (s_gsv_first_cycle) { s_gsv_first_cycle = false; s_gsv_n = 0; return; }
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
    static uint32_t task_start_ms  = 0;
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
        if (task_start_ms == 0) {
            task_start_ms = now;
            // Auch den Status-Zeitstempel mitziehen. Bei 0 belassen läge die
            // erste 30-s-Frist sofort in der Vergangenheit und die Statuszeile
            // feuerte im selben Augenblick — also bevor ein vollständiger
            // GSV-Zyklus vorliegt, mit entsprechend unsinniger Satellitenzahl.
            last_status_ms = now;
        }

        // Kein NMEA nach 5s → warnen. Gemessen ab Start dieser Task, nicht ab
        // Boot: gps_ext_init() braucht mit Baudsuche und Konfiguration mehrere
        // Sekunden, und gegen millis() gerechnet feuerte die Warnung dadurch
        // sofort beim Start — direkt bevor die erste NMEA-Zeile eintraf.
        if (!s_nmea_valid && (now - task_start_ms) > 5000
                          && (now - last_status_ms) > 5000) {
            last_status_ms = now;
            syslog("GPS_EXT", "WARNUNG: Keine NMEA-Daten — Verdrahtung prüfen");
        }

        // Positions-Cache verfallen lassen, wenn das Modul verstummt.
        // Siehe GPS_EXT_STALE_MS — ohne das würde eine eingefrorene Position
        // weiter erfasst und gesendet.
        if (s_last_pos_ms && gps_valid()
                          && (now - s_last_pos_ms) > GPS_EXT_STALE_MS) {
            gps_invalidate();
            char m[72];
            snprintf(m, sizeof(m), "Keine Position seit %lus — Cache verworfen",
                     (unsigned long)((now - s_last_pos_ms) / 1000));
            syslog("GPS_EXT", m);
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

        // Erster Fix. Erst melden, wenn die Satellitenübersicht vollständig ist —
        // der Fix kommt inzwischen schneller als der erste komplette GSV-Zyklus,
        // und ein Teilzyklus ergäbe Meldungen wie "1 sichtbar / 12 im Fix".
        // Als Prüfung dient die logische Untergrenze: sichtbar kann nie kleiner
        // sein als die Zahl der im Fix verwendeten Satelliten. Kostet rund eine
        // Sekunde in der Meldung, nicht im Fix selbst.
        if (!fix_logged && gps_valid() && s_sats_visible >= s_sat_count) {
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

    // Versorgung zuerst — der Sleep-Pfad hat DC5 abgeschaltet. pmu_init() holt
    // das zwar ohnehin nach, aber gps_ext_wake() wird auch aus gps_ext_init()
    // heraus gerufen, und dort ist nichts garantiert. Doppelt einschalten
    // schadet nicht: pmu_set_ext_power() prüft nicht, sondern setzt.
    if (!pmu_is_dc5_on()) {
        pmu_set_ext_power(true);
        delay(150);          // Anlauf der Schiene abwarten, bevor Pins treiben
    }

    // Nach Deep Sleep steht die erkannte Rate noch im RTC-Speicher.
    s_gps_uart.begin(s_baud ? s_baud : (uint32_t)GPS_EXT_BAUD, SERIAL_8N1,
                     GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);

    // Aus dem Software-Standby weckt jede Flanke auf RX — es gibt kein
    // besonderes Weckbyte, schon das Startbit genügt. Das Byte selbst geht
    // dabei verloren, weil der UART-Block beim Eintreffen der Flanke noch
    // nicht getaktet ist. Deshalb ein Wegwerf-Byte, das nie Teil einer echten
    // Nachricht sein darf. 0xFF ist üblich, weil es nach dem Startbit die
    // wenigsten Flanken erzeugt.
    // Beim Kaltstart schadet es nichts: das Modul verwirft ein Byte, das weder
    // NMEA noch UBX ist. (Ein einzelnes Byte ist auch unkritisch für die
    // Abschaltung des RX-Pfades, die erst nach über 100 Framing-Fehlern
    // innerhalb einer Sekunde greift.)
    s_gps_uart.write(0xFF);
    s_gps_uart.flush();
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

    if (answering) {
        // Identität nur beim Kaltstart — die ändert sich nicht.
        if (!sleep_was_deep()) log_mon_ver();

        // Konfiguration dagegen bei JEDEM Start prüfen. ensure_key() liest erst
        // und schreibt nur bei Abweichung, im Normalfall kostet das also nur ein
        // paar Abfragen. Nötig wird es, sobald der Sleep-Pfad dem Modul die
        // Versorgung nimmt: dann ist die RAM-Ebene weg und mit ihr der
        // L5-Health-Override, der genau dort liegt. Nur beim Kaltstart zu
        // konfigurieren würde das Dual-Band nach dem ersten Sleep still
        // wieder abschalten.
        configure_gnss();
        enable_gps_l5();
        enable_aop();

        // Warm oder kalt? Danach richtet sich nur noch, ob dem Modul ein
        // Startpunkt injiziert werden muss.
        //
        // Die Einschwingsperre bleibt in BEIDEN Faellen bestehen. Ein frueherer
        // Stand setzte sie beim Warmstart auf 0, weil eine gueltige Modulzeit
        // als Beleg fuer eine brauchbare Position galt. Das ist falsch: gueltige
        // Zeit beweist nur, dass der Stuetzpuffer gehalten hat, nicht dass die
        // Positionsloesung eingeschwungen ist. Am Geraet gemessen, Modul
        // ortsfest, Abstand zur wahren Position:
        //
        //     + 0s fixType=2 sats= 3  Abstand 1730 m  hAcc=  244 m
        //     + 4s fixType=2 sats= 5  Abstand 4658 m  hAcc= 2937 m
        //     + 8s fixType=3 sats= 6  Abstand 3874 m  hAcc= 1489 m
        //     +20s fixType=3 sats= 7  Abstand   44 m  hAcc=  295 m
        //     +28s fixType=3 sats=13  Abstand    9 m  hAcc=   54 m
        //
        // Bei +8 s steht ein gueltiger 3D-Fix, die Position liegt aber vier
        // Kilometer daneben — und wurde ohne Sperre sofort erfasst und per MQTT
        // verschickt. Brauchbar wird sie erst nach rund 28 s. Gerechnet ab dem
        // ersten gueltigen RMC treffen die 20 s genau diesen Punkt.
        //
        // Besser waere ein Gate auf die Genauigkeitsschaetzung hAcc aus
        // UBX-NAV-PVT statt einer festen Wartezeit — das gaebe frueher frei,
        // wenn die Loesung schon gut ist. Dafuer muesste NAV-PVT im Lese-Task
        // mitgeparst werden; eigener Schritt, eigener Nachweis.
        s_settle_ms = GPS_EXT_SETTLE_MS;
        if (module_time_valid()) {
            syslog("GPS_EXT", "Warmstart — Modulzeit gueltig, Sperre 20s aktiv");
        } else {
            syslog("GPS_EXT", "Kaltstart — Backup leer, Sperre 20s aktiv");
            inject_pos();   // grober Startpunkt aus NVS verkuerzt die Suche
        }
    }
    // PPS-LED wieder an (war im Sleep deaktiviert)
    if (answering) enable_timepulse();

    // s_ok wird erst nach erstem validen NMEA-Paket gesetzt (in process_line)
    { char m[96]; snprintf(m, sizeof(m), "Ext. GPS — UART GPIO%d/%d %luBd (warte auf Fix)",
                           GPS_EXT_RX_PIN, GPS_EXT_TX_PIN,
                           (unsigned long)(s_baud ? s_baud : (uint32_t)GPS_EXT_BAUD));
      syslog("GPS_EXT", m); }
    xTaskCreatePinnedToCore(gps_ext_task, "GPS_EXT", 4096, NULL, 2, &s_task, 1);
}

// ── Software-Standby: UBX-RXM-PMREQ ──────────────────────
// Der DAN-F10N kennt genau zwei sparsame Zustände — einen zyklischen
// Stromsparmodus gibt es auf F10 SPG 6.00 nicht, die ganze CFG-PM-Gruppe fehlt
// dieser Firmware. Bleiben Software-Standby (VCC an, 49 µA) und Hardware-Backup
// (VCC weg, 31 µA aus der Stützzelle). Gewählt ist der Standby, weil das
// Hardware-Backup an der Knopfzelle des Boards hängt: eine ML414H mit 0,8 mAh
// garantierter Kapazität trägt bei 31 µA rund einen Tag, bei −20 °C nur etwa
// sechs Stunden. Danach ist BBR verloren — samt Ephemeriden und Uhrzeit, also
// Kaltstart, und das ausgerechnet nach langem Parken. Dazu darf bei fehlendem
// VCC laut Datenblatt kein Pin mehr getrieben werden; unsere UART-TX-Leitung
// würde in ein unversorgtes Modul einspeisen. Der Standby bringt 21 mA → 49 µA
// ohne diese Risiken; die restlichen 18 µA sind den Ärger nicht wert.
//
// Payload ist 16 Byte mit version = 0. Die 8-Byte-Form ist M8-Erbe und kommt in
// der F10-Dokumentation nicht vor — nicht senden. Achtung auch bei der
// Bedeutung von duration: auf M8 hieß 0 "unbegrenzt", auf F10 heißt es
// "auf ein Wecksignal an einem Pin warten". Ergebnis ist dasselbe, die
// Begründung eine andere.
//
// RXM-PMREQ wird NICHT quittiert — auf ein ACK zu warten liefe immer in einen
// Timeout und sähe im Log wie ein Fehler aus.
static void gps_ext_standby() {
    uint8_t p[16] = {};
    p[0]  = 0x00;      // version = 0 (einzige auf F10 dokumentierte Form)
                       // p[1..3]  reserved
                       // p[4..7]  duration = 0 → auf Wecksignal warten
    p[8]  = 0x02;      // flags: Bit1 = backup
    p[12] = 0x08;      // wakeupSources: Bit3 = uartrx
                       // extint0 wäre Bit5, aber welcher der beiden EXTINT-
                       // Indizes dem einzigen EXTINT-Pin des DAN-F10N
                       // entspricht, ist nirgends dokumentiert. uartrx ist
                       // eindeutig und genügt.
    ubx_send(0x02, 0x41, p, 16);
    s_gps_uart.flush();
    delay(100);
}

// ── GPS schlafen legen: Versorgung abschalten ────────────
// DC5 wird abgeschaltet. Das Modul geht damit ins Hardware-Backup: VCC weg,
// nur V_BCKP aus der Stützzelle des Boards hält RTC und BBR. Am Gerät
// nachgemessen ("gps powercycle 10", DC5-Register gegengeprüft): das Backup
// trägt, danach steht der 3D-Fix nach gut vier Sekunden.
//
// Bewusst kein RXM-PMREQ davor: der Software-Standby setzt voraus, dass VCC
// anliegt, und wäre hier gegenstandslos. Er bleibt als gps_ext_standby() im
// Code, falls die Versorgung doch einmal stehenbleiben soll — der Serial-Befehl
// "gps standby" fährt ihn.
//
// Reihenfolge ist wichtig: erst UART schließen und die Pins hochohmig machen,
// dann die Versorgung nehmen. Bei fehlendem VCC darf laut Datenblatt kein Pin
// des Moduls mehr getrieben werden, sonst speist unsere TX-Leitung in ein
// unversorgtes Modul ein.
//
// Hält die Stützzelle nicht durch — sie trägt rund einen Tag, bei Frost
// weniger — geht BBR verloren und es wird ein Kaltstart. Der ist abgefedert:
// gps_ext_init() erkennt den Fall und injiziert Zeit aus der DS1307 und die
// letzte Position aus NVS, bevor es auf den Fix wartet.
void gps_ext_sleep() {
    if (!GPS_EXT_ENABLED) return;
    save_last_pos(true);     // Position sichern, solange sie noch gilt
    disable_timepulse();     // PPS-LED aus — hängt am TIMEPULSE-Pin
    s_gps_uart.flush();
    s_gps_uart.end();
    pinMode(GPS_EXT_TX_PIN, INPUT);
    pinMode(GPS_EXT_RX_PIN, INPUT);
    pmu_set_ext_power(false);
    syslog("GPS_EXT", "Versorgung aus (DC5) — Backup haelt RTC + Ephemeriden");
}

// ── Bench-Test: Standby und Aufwachen nachmessen ─────────
// Serial-Befehl "gps standby". Prüft, was sich sonst nur im Deep Sleep zeigt —
// und den kann man am Schreibtisch schlecht auslösen, weil das Aufwachen an
// einem Bewegungsinterrupt hängt. Der Lese-Task wird dafür angehalten, sonst
// zählt er die Zeilen mit weg, die hier gezählt werden sollen.
void gps_ext_standby_test() {
    if (!GPS_EXT_ENABLED) { Serial.println("[GPS] ext. GPS ist aus"); return; }
    if (s_task) vTaskSuspend(s_task);

    auto count_nmea = [](uint32_t ms) {
        uint32_t start = millis();
        int      n     = 0;
        while ((millis() - start) < ms) {
            if (s_gps_uart.available() && s_gps_uart.read() == '$') n++;
            else delay(1);
        }
        return n;
    };

    Serial.println("[GPS] --- Standby-Test ---");
    Serial.printf("[GPS] vorher:  %d NMEA-Zeilen in 2s\n", count_nmea(2000));

    gps_ext_standby();
    delay(500);                       // Modul in den Standby laufen lassen
    int during = count_nmea(3000);
    Serial.printf("[GPS] Standby: %d NMEA-Zeilen in 3s%s\n", during,
                  during == 0 ? "  → Modul ist im Standby"
                              : "  → sendet weiter, Standby NICHT erreicht!");

    uint32_t t0 = millis();
    s_gps_uart.write(0xFF);           // Weckflanke; dieses Byte geht verloren
    s_gps_uart.flush();
    while (s_gps_uart.available()) s_gps_uart.read();
    int      lines = 0;
    uint32_t first = 0;
    while ((millis() - t0) < 5000 && lines < 3) {
        if (s_gps_uart.available()) {
            if (s_gps_uart.read() == '$') { if (!first) first = millis(); lines++; }
        } else delay(1);
    }
    if (lines)
        Serial.printf("[GPS] Wake:    erste Zeile nach %lu ms\n",
                      (unsigned long)(first - t0));
    else
        Serial.println("[GPS] Wake:    keine Daten nach 5s — Aufwecken fehlgeschlagen!");

    // Der Standby löscht die RAM-Ebene. Ob sie dabei aus BBR neu aufgebaut wird,
    // sagt u-blox nicht — hier steht es schwarz auf weiß. Ist die Antwort nein,
    // trägt allein das erneute Konfigurieren in gps_ext_init() das Dual-Band
    // über den Sleep; ist sie ja, ist das nur noch die zweite Sicherung.
    delay(500);
    uint8_t ram = 0, bbr = 0;
    bool okr = cfg_read_u1(0x10320001UL, ram, 0);
    bool okb = cfg_read_u1(0x10320001UL, bbr, 1);
    Serial.printf("[GPS] L5-Override nach Wake: RAM=%s  BBR=%s\n",
                  okr ? (ram ? "1" : "0") : "keine Antwort",
                  okb ? (bbr ? "1" : "0") : "keine Antwort");
    if (s_task) vTaskResume(s_task);
    Serial.println("[GPS] --- Ende ---");
}

// ── GSV-Sätze und ihre Auswertung zeigen ─────────────────
// Serial-Befehl "gps gsv". Schneidet rohe GSV-Sätze mit und zeigt daneben, wie
// viele Satelliten der Satz laut numSV/msgNum trägt und welche IDs daraus
// gelesen werden. Damit ist sofort sichtbar, wenn wieder eine signalId als
// Satellit gezählt würde — genau der Fehler, den diese Auswertung aufgedeckt hat.
void gps_ext_gsv_proof() {
    if (!GPS_EXT_ENABLED) { Serial.println("[GPS] ext. GPS ist aus"); return; }
    Serial.printf("[GPS] --- GSV-Nachweis --- Parser zaehlt gerade: %d sichtbar\n",
                  s_sats_visible);
    if (s_task) vTaskSuspend(s_task);
    while (s_gps_uart.available()) s_gps_uart.read();

    char     line[128];
    uint8_t  pos = 0;
    uint32_t t0  = millis();
    int      shown = 0;
    while ((millis() - t0) < 4000 && shown < 24) {
        if (!s_gps_uart.available()) { delay(2); continue; }
        char c = (char)s_gps_uart.read();
        if (c == '$') pos = 0;
        if (pos >= sizeof(line) - 1) { pos = 0; continue; }
        line[pos++] = c;
        if (c != '\n') continue;
        line[pos] = '\0'; pos = 0;
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = '\0';
        if (l < 6 || !nmea_checksum_ok(line)) continue;
        if (strncmp(line + 3, "GSV", 3) != 0) continue;

        // Dieselbe Logik wie parse_gsv() — sonst prüft der Nachweis sich selbst.
        char copy[128];
        strncpy(copy, line, sizeof(copy) - 1); copy[sizeof(copy) - 1] = '\0';
        int  n = gsv_sats_in_msg(copy);
        char gelesen[64] = {};
        for (int i = 0; i < n; i++) {
            const char* s = nmea_field(copy, 4 + i * 4);
            if (!s || s[0] == '\0' || s[0] == ',' || s[0] == '*') break;
            char one[16];
            snprintf(one, sizeof(one), "%s%d", gelesen[0] ? "," : "", atoi(s));
            strncat(gelesen, one, sizeof(gelesen) - strlen(gelesen) - 1);
        }
        Serial.printf("  %s\n      -> %d Sat(s) laut numSV/msgNum, gelesen: %s\n",
                      line, n, gelesen);
        shown++;
    }
    if (s_task) vTaskResume(s_task);
    Serial.println("[GPS] --- Ende ---");
}

// ── Rohe GGA/RMC-Saetze zeigen ───────────────────────────
// Serial-Befehl "gps raw". Zeigt, was das Modul wirklich meldet, und daneben
// die beiden Felder, an denen die Firmware haengt: GGA-Feld 6 (Fix-Qualitaet,
// speist s_sat_count) und RMC-Feld 2 (Status A/V). Bleibt s_sat_count auf -1,
// liegt es genau an GGA-Feld 6.
void gps_ext_raw_proof() {
    if (!GPS_EXT_ENABLED) { Serial.println("[GPS] ext. GPS ist aus"); return; }
    Serial.printf("[GPS] --- Roh --- s_sat_count=%d  s_sats_visible=%d  gps_valid=%d\n",
                  s_sat_count, s_sats_visible, gps_valid() ? 1 : 0);
    if (s_task) vTaskSuspend(s_task);
    while (s_gps_uart.available()) s_gps_uart.read();

    char line[128]; uint8_t pos = 0; int shown = 0;
    uint32_t t0 = millis();
    while ((millis() - t0) < 6000 && shown < 12) {
        if (!s_gps_uart.available()) { delay(2); continue; }
        char c = (char)s_gps_uart.read();
        if (c == '$') pos = 0;
        if (pos >= sizeof(line) - 1) { pos = 0; continue; }
        line[pos++] = c;
        if (c != '\n') continue;
        line[pos] = '\0'; pos = 0;
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = '\0';
        if (l < 6 || !nmea_checksum_ok(line)) continue;
        bool gga = (strncmp(line + 3, "GGA", 3) == 0);
        bool rmc = (strncmp(line + 3, "RMC", 3) == 0);
        if (!gga && !rmc) continue;
        char copy[128];
        strncpy(copy, line, sizeof(copy) - 1); copy[sizeof(copy)-1] = '\0';
        if (gga)
            Serial.printf("  %s\n      GGA Qualitaet=[%s] Sats=[%s]\n",
                          line, nmea_field(copy, 6), nmea_field(copy, 7));
        else
            Serial.printf("  %s\n      RMC Status=[%c]\n", line, nmea_field(copy, 2)[0]);
        shown++;
    }
    if (s_task) vTaskResume(s_task);
    Serial.println("[GPS] --- Ende ---");
}

// ── Eine Konfigurationsgruppe ausgeben ───────────────────
// Serial-Befehl "gps cfg <hex>", z.B. "gps cfg 05" für CFG-TP (Zeitimpuls).
// Der Lese-Task wird angehalten, sonst frisst er Bytes aus der Antwort.
void gps_ext_dump_group(uint16_t group) {
    if (!GPS_EXT_ENABLED) { Serial.println("[GPS] ext. GPS ist aus"); return; }
    if (s_task) vTaskSuspend(s_task);
    char name[16];
    snprintf(name, sizeof(name), "CFG-0x%02X", group);
    dump_cfg_group(name, group);
    if (s_task) vTaskResume(s_task);
}

// ── Bench-Test: Versorgung wegnehmen und wiedergeben ─────
// Serial-Befehl "gps powercycle <sek>". Stellt genau den Fall nach, der beim
// Stecker-Ziehen ohne Akku eintritt: DC5 aus, Modul komplett stromlos, nur die
// Stützzelle des Boards hält V_BCKP. Beantwortet damit, ob BBR die Pause
// übersteht — und wie schnell danach wieder ein Fix steht.
void gps_ext_powercycle_test(uint32_t off_s) {
    if (!GPS_EXT_ENABLED) { Serial.println("[GPS] ext. GPS ist aus"); return; }
    if (off_s == 0 || off_s > 600) off_s = 10;

    Serial.printf("[GPS] --- Powercycle-Test: %lus ohne Versorgung ---\n",
                  (unsigned long)off_s);
    Serial.printf("[GPS] vorher: %d sichtbar / %d im Fix\n",
                  s_sats_visible, s_sat_count);

    // Der Lese-Task läuft während der Stromlosigkeit bewusst WEITER — nur so
    // greift die Verfallsprüfung des Positions-Caches, die genau hier zu zeigen
    // ist. Angehalten wird er erst für die Messungen nach dem Einschalten, wo er
    // sonst Bytes aus den UBX-Antworten fräße. Der UART ist zu diesem Zeitpunkt
    // geschlossen; available() liefert dann 0, der Task dreht also leer.

    // Erst den UART schliessen und TX hochohmig machen. Bei fehlendem VCC darf
    // laut Datenblatt kein Pin des Moduls mehr getrieben werden — sonst speist
    // unsere TX-Leitung in ein unversorgtes Modul ein.
    s_gps_uart.flush();
    s_gps_uart.end();
    pinMode(GPS_EXT_TX_PIN, INPUT);
    pinMode(GPS_EXT_RX_PIN, INPUT);

    pmu_set_ext_power(false);
    delay(200);
    // Gegenprobe: schaltet die Schiene wirklich ab? Sonst misst der Test nur
    // einen UART-Neustart und nicht den Stromausfall, den er nachstellen soll.
    Serial.printf("[GPS] DC5 nach dem Abschalten laut PMU-Register: %s\n",
                  pmu_is_dc5_on() ? "IMMER NOCH AN — Test misst nicht, was er soll!"
                                  : "aus");

    // Während der Stromlosigkeit den GPS-Cache beobachten. Das Modul kann jetzt
    // nichts mehr liefern — was hier trotzdem als gültig gemeldet wird, geht in
    // die Telemetrie und über MQTT hinaus.
    for (uint32_t i = 0; i < off_s; i++) {
        delay(1000);
        if (i % 5 == 4 || i == 0) {
            GpsSnapshot g = gps_snapshot();
            Serial.printf("[GPS] +%2lus ohne Strom: gps_valid=%s  %.5f / %.5f  %.1f km/h\n",
                          (unsigned long)(i + 1), g.valid ? "JA" : "nein",
                          g.lat, g.lon, g.speed_kmh);
        }
    }
    // Ab hier wird wieder über den UART gemessen — Task anhalten.
    if (s_task) vTaskSuspend(s_task);
    pmu_set_ext_power(true);

    uint32_t t0 = millis();
    s_gps_uart.begin(s_baud ? s_baud : (uint32_t)GPS_EXT_BAUD, SERIAL_8N1,
                     GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);

    // Gemessen wird ueber gezielte NAV-PVT-Abfragen, nicht durch Mithoeren am
    // UART: der GPS-Task liest parallel mit und frisst Zeilen weg, eine
    // mitlesende Schleife misst deshalb nur, wann sie zufaellig etwas erwischt.
    // Eine Abfrage mit passender Antwort ist eindeutig.
    // NAV-PVT: Offset 11 = valid (Bit0 Datum, Bit1 Zeit), 20 = fixType, 23 = numSV.
    auto pvt = [](uint8_t& valid, uint8_t& fix, uint8_t& sats) -> bool {
        for (uint8_t t = 0; t < 4; t++) {
            while (s_gps_uart.available()) s_gps_uart.read();
            ubx_send(0x01, 0x07, nullptr, 0);
            int len = ubx_recv(0x01, 0x07, s_ubx_buf, sizeof(s_ubx_buf), 1200);
            if (len >= 24) {
                valid = s_ubx_buf[11]; fix = s_ubx_buf[20]; sats = s_ubx_buf[23];
                return true;
            }
            delay(150);
        }
        return false;
    };

    // ZUERST fragen, DANN injizieren — sonst misst die Pruefung die eigene
    // Injektion und die Aussage ueber die Stuetzzelle waere wertlos.
    uint8_t v = 0, f = 0, n = 0;
    delay(1200);
    if (pvt(v, f, n)) {
        Serial.printf("[GPS] direkt nach dem Einschalten: Datum=%s Zeit=%s "
                      "fixType=%u Sats=%u  → Backup %s\n",
                      (v & 0x01) ? "gueltig" : "UNGUELTIG",
                      (v & 0x02) ? "gueltig" : "UNGUELTIG", f, n,
                      (v & 0x03) == 0x03 ? "hat gehalten (Stuetzzelle traegt)"
                                         : "VERLOREN (keine Stuetzzelle)");
    } else {
        Serial.println("[GPS] keine NAV-PVT-Antwort direkt nach dem Einschalten");
    }

    // Jetzt der Kaltstartpfad, wie ihn gps_ext_init() nach einem Stromausfall
    // ginge: Zeit aus der DS1307 und die letzte bekannte Position injizieren.
    inject_time();
    inject_pos();

    // NACHWEIS: wandert die Position nach dem Einschalten noch? NAV-PVT liefert
    // lon (Offset 24), lat (28) und hAcc (40, Genauigkeitsschaetzung in mm).
    // Damit laesst sich sehen, ob ein frueher Fix schon brauchbar ist — und ab
    // welchem hAcc er es waere.
    {
        uint32_t tm0 = millis();
        while ((millis() - tm0) < 75000UL) {
            while (s_gps_uart.available()) s_gps_uart.read();
            ubx_send(0x01, 0x07, nullptr, 0);
            int len = ubx_recv(0x01, 0x07, s_ubx_buf, sizeof(s_ubx_buf), 1200);
            if (len >= 48) {
                int32_t lon = (int32_t)((uint32_t)s_ubx_buf[24] | ((uint32_t)s_ubx_buf[25]<<8)
                              | ((uint32_t)s_ubx_buf[26]<<16) | ((uint32_t)s_ubx_buf[27]<<24));
                int32_t la  = (int32_t)((uint32_t)s_ubx_buf[28] | ((uint32_t)s_ubx_buf[29]<<8)
                              | ((uint32_t)s_ubx_buf[30]<<16) | ((uint32_t)s_ubx_buf[31]<<24));
                uint32_t hacc = (uint32_t)s_ubx_buf[40] | ((uint32_t)s_ubx_buf[41]<<8)
                              | ((uint32_t)s_ubx_buf[42]<<16) | ((uint32_t)s_ubx_buf[43]<<24);
                Serial.printf("[GPS] +%2lus fix=%u sats=%2u  %.5f / %.5f  hAcc=%lu m\n",
                              (unsigned long)((millis()-tm0)/1000), s_ubx_buf[20], s_ubx_buf[23],
                              la / 1e7, lon / 1e7, (unsigned long)(hacc / 1000));
            }
            delay(3000);
        }
    }

    // Bis zum 3D-Fix pollen (fixType 3). 2 waere ein 2D-Fix und noch zu grob.
    uint32_t t_fix = 0;
    while ((millis() - t0) < 120000) {
        if (pvt(v, f, n) && f >= 3) { t_fix = millis(); break; }
        delay(500);
    }
    if (t_fix)
        Serial.printf("[GPS] 3D-Fix nach %lu ms (%u Sats)  → %s\n",
                      (unsigned long)(t_fix - t0), n,
                      (t_fix - t0) < 10000 ? "schnell, Aiding hat gewirkt"
                                           : "langsam, echter Kaltstart");
    else
        Serial.println("[GPS] binnen 120s kein 3D-Fix");
    Serial.println("[GPS] Hinweis: die Einschwingsperre der Firmware ist hier nicht");
    Serial.println("[GPS] enthalten — gemessen ist die Rohzeit des Moduls.");
    if (s_task) vTaskResume(s_task);
    Serial.println("[GPS] --- Ende ---");
}

bool gps_ext_ok()        { return s_ok; }
int  gps_ext_sat_count()   { return s_sat_count; }
int  gps_ext_sat_visible() { return s_sats_visible; }
