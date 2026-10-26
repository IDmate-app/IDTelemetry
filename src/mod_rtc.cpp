#include "mod_rtc.h"
#include "mod_sleep.h"
#include "mod_logs.h"
#include <Wire.h>
#include "config.h"

// ============================================================
//  mod_rtc - DS1307 RTC
// ============================================================

#define DS1307_ADDR 0x68

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static uint32_t s_ref_millis    = 0;     // millis() zum Zeitpunkt des letzten RTC-Abrufs
static uint32_t s_ref_ms_of_day = 0;    // ms seit Mitternacht zum Zeitpunkt des Abrufs
static bool     s_rtc_synced    = false; // false = RTC noch nie gelesen
static uint32_t s_ref_unix      = 0;    // Unix-Timestamp beim letzten Sync
static bool     s_has_date      = false; // true wenn Datum gesetzt wurde

void rtc_init() {
    // I2C Bus-Recovery: Falls SDA nach Deep Sleep LOW hängt,
    // 9 Clock-Pulse senden um Slave-Zustand zu resetten
    pinMode(RTC_SCL_PIN, OUTPUT);
    pinMode(RTC_SDA_PIN, INPUT_PULLUP);
    for (int i = 0; i < 9; i++) {
        digitalWrite(RTC_SCL_PIN, HIGH); delayMicroseconds(5);
        digitalWrite(RTC_SCL_PIN, LOW);  delayMicroseconds(5);
    }
    digitalWrite(RTC_SCL_PIN, HIGH); delayMicroseconds(5);
    // STOP-Condition: SDA LOW→HIGH während SCL HIGH
    pinMode(RTC_SDA_PIN, OUTPUT);
    digitalWrite(RTC_SDA_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(RTC_SCL_PIN, HIGH); delayMicroseconds(5);
    digitalWrite(RTC_SDA_PIN, HIGH); delayMicroseconds(5);

    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    // CH-Bit pruefen — wenn gesetzt war die Uhr gestoppt (Batterie leer/neu)
    Wire.beginTransmission(DS1307_ADDR);
    Wire.write(0x00);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)DS1307_ADDR, (uint8_t)1);
    if (!Wire.available()) {
        syslog("RTC", "FEHLER: DS1307 nicht gefunden — Lötstelle/I2C prüfen");
        return;
    }
    uint8_t sec = Wire.read();
    if (sec & 0x80) {
        Wire.beginTransmission(DS1307_ADDR);
        Wire.write(0x00);
        Wire.write(sec & 0x7F);
        Wire.endTransmission();
        syslog("RTC", "DS1307 gefunden · Batterie leer/neu — Uhr gestartet");
    } else {
        char _m[48];
        snprintf(_m, sizeof(_m), "DS1307 OK · %s", rtc_time_str().c_str());
        syslog("RTC", _m);
    }
}

bool rtc_get_time(int& h, int& m, int& s) {
    Wire.beginTransmission(DS1307_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom((uint8_t)DS1307_ADDR, (uint8_t)3);
    if (Wire.available() < 3) return false;
    s = bcd2dec(Wire.read() & 0x7F);
    m = bcd2dec(Wire.read() & 0x7F);
    h = bcd2dec(Wire.read() & 0x3F);
    return true;
}

bool rtc_set_time(int h, int m, int s) {
    Wire.beginTransmission(DS1307_ADDR);
    Wire.write(0x00);
    Wire.write(dec2bcd(s) & 0x7F);  // CH=0 -> Uhr laeuft
    Wire.write(dec2bcd(m));
    Wire.write(dec2bcd(h));
    bool ok = Wire.endTransmission() == 0;
    if (ok) {
        s_ref_ms_of_day = (uint32_t)(h * 3600 + m * 60 + s) * 1000;
        s_ref_millis    = millis();
        s_rtc_synced    = true;
    }
    return ok;
}

bool rtc_get_datetime(int& y, int& mo, int& d, int& h, int& mi, int& s) {
    Wire.beginTransmission(DS1307_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom((uint8_t)DS1307_ADDR, (uint8_t)7);
    if (Wire.available() < 7) return false;
    s  = bcd2dec(Wire.read() & 0x7F);  // Reg 0: Sekunden
    mi = bcd2dec(Wire.read() & 0x7F);  // Reg 1: Minuten
    h  = bcd2dec(Wire.read() & 0x3F);  // Reg 2: Stunden
    Wire.read();                        // Reg 3: Wochentag (ignoriert)
    d  = bcd2dec(Wire.read() & 0x3F);  // Reg 4: Tag
    mo = bcd2dec(Wire.read() & 0x1F);  // Reg 5: Monat
    y  = 2000 + bcd2dec(Wire.read());   // Reg 6: Jahr (00-99)
    return true;
}

bool rtc_set_datetime(int y, int mo, int d, int h, int mi, int s) {
    // DS1307 Register 0-6: Sek, Min, Std, Wochentag, Tag, Monat, Jahr
    // Wochentag berechnen (Zeller) — DS1307 braucht 1-7
    // Vereinfachte Berechnung (Tomohiko Sakamoto)
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = y; if (mo < 3) yy--;
    int dow = (yy + yy/4 - yy/100 + yy/400 + t[mo-1] + d) % 7;
    dow = dow == 0 ? 7 : dow;  // DS1307: 1=Mo..7=So

    Wire.beginTransmission(DS1307_ADDR);
    Wire.write(0x00);
    Wire.write(dec2bcd(s) & 0x7F);      // CH=0
    Wire.write(dec2bcd(mi));
    Wire.write(dec2bcd(h));
    Wire.write(dec2bcd(dow));
    Wire.write(dec2bcd(d));
    Wire.write(dec2bcd(mo));
    Wire.write(dec2bcd(y - 2000));
    bool ok = Wire.endTransmission() == 0;
    if (ok) {
        s_ref_ms_of_day = (uint32_t)(h * 3600 + mi * 60 + s) * 1000;
        s_ref_millis    = millis();
        s_rtc_synced    = true;
        s_has_date      = true;
        // Unix-Timestamp berechnen
        // Tage seit 1970-01-01 (einfache Formel, korrekt 2000-2099)
        uint32_t days = 0;
        for (int yr = 1970; yr < y; yr++) {
            days += (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0)) ? 366 : 365;
        }
        static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        for (int mm = 1; mm < mo; mm++) {
            days += mdays[mm];
            if (mm == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days++;
        }
        days += d - 1;
        s_ref_unix = days * 86400UL + h * 3600UL + mi * 60UL + s;
    }
    return ok;
}

// Unix-Timestamp (Sekunden seit Epoch)
uint32_t rtc_unix_timestamp() {
    if (!s_has_date) return 0;
    uint32_t elapsed_s = (millis() - s_ref_millis) / 1000;
    return s_ref_unix + elapsed_s;
}

// Unix-Timestamp in Millisekunden für InfluxDB
uint64_t rtc_unix_ms() {
    if (!s_has_date) return 0;
    uint32_t elapsed = millis() - s_ref_millis;
    return (uint64_t)s_ref_unix * 1000ULL + (uint64_t)elapsed;
}

bool rtc_is_running() {
    Wire.beginTransmission(DS1307_ADDR);
    Wire.write(0x00);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)DS1307_ADDR, (uint8_t)1);
    if (!Wire.available()) return false;
    return !(Wire.read() & 0x80);
}

String rtc_time_str() {
    int h, m, s;
    if (!rtc_get_time(h, m, s)) return "--:--:--";
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return String(buf);
}

// Felder eines DS1307-Reads plausibilisieren (FIXES B.11 — real beobachtete
// Glitches: Jahr 2093, Monat 25). Gleiche Schranken wie inject_time().
static bool rtc_fields_plausible(int y, int mo, int d, int h, int m, int s) {
    return y >= 2024 && y <= 2040 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
           h >= 0 && h <= 23 && m >= 0 && m <= 59 && s >= 0 && s <= 59;
}

// Unix-Sekunden aus (plausibilisierten) Feldern.
static uint32_t rtc_to_unix(int y, int mo, int d, int h, int m, int s) {
    uint32_t days = 0;
    for (int yr = 1970; yr < y; yr++)
        days += (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0)) ? 366 : 365;
    static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    for (int mm = 1; mm < mo; mm++) {
        days += mdays[mm];
        if (mm == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days++;
    }
    days += d - 1;
    return days * 86400UL + h * 3600UL + m * 60UL + s;
}

static void rtc_do_sync() {
    int y, mo, d, h, m, s;
    if (!rtc_get_datetime(y, mo, d, h, m, s)) return;

    // FIXES B.11: Glitch mit unsinnigen Feldern verwerfen — sonst verschiebt er
    // s_ref_unix und damit bis zu 60 s lang ALLE Row-Timestamps (Empfänger
    // sortiert/filtert nach Zeit). Alte Referenz bleibt unangetastet.
    if (!rtc_fields_plausible(y, mo, d, h, m, s)) return;

    uint32_t cand_unix = rtc_to_unix(y, mo, d, h, m, s);

    // Sprung-Erkennung: weicht der Kandidat stark von der erwarteten Zeit
    // (alte Referenz + vergangene millis) ab, einmal nachlesen zur Bestätigung.
    // So werden transiente Glitches gefiltert, eine echte Korrektur (NTP/GPS
    // setzt die RTC) aber akzeptiert, weil dann zwei Reads übereinstimmen.
    if (s_has_date) {
        uint32_t elapsed  = (millis() - s_ref_millis) / 1000UL;
        uint32_t expected = s_ref_unix + elapsed;
        uint32_t diff     = (cand_unix > expected) ? (cand_unix - expected) : (expected - cand_unix);
        if (diff > 5) {
            delay(5);
            int y2, mo2, d2, h2, m2, s2;
            if (!rtc_get_datetime(y2, mo2, d2, h2, m2, s2)) return;
            if (!rtc_fields_plausible(y2, mo2, d2, h2, m2, s2)) return;
            int hms1 = h  * 3600 + m  * 60 + s;
            int hms2 = h2 * 3600 + m2 * 60 + s2;
            if (abs(hms1 - hms2) > 2) {
                syslog("RTC", "Sync verworfen — Zeitsprung nicht bestätigt (Glitch?)");
                return;
            }
        }
    }

    // Akzeptiert — Referenz übernehmen.
    s_ref_ms_of_day = (uint32_t)(h * 3600 + m * 60 + s) * 1000;
    s_ref_millis    = millis();
    s_rtc_synced    = true;
    s_ref_unix      = cand_unix;
    s_has_date      = true;
}

static void rtc_sync_task(void*) {
    while (!g_shutdown) {
        rtc_do_sync();
        // In 600 Schritten à 100ms warten — reagiert schneller auf Shutdown
        for (int i = 0; i < 600 && !g_shutdown; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    Serial.println("[RTC] Sync-Task beendet (Shutdown)");
    vTaskDelete(NULL);
}

void rtc_time_sync_task_init() {
    rtc_do_sync();  // sofort beim Start einmal synchronisieren
    xTaskCreatePinnedToCore(rtc_sync_task, "RTC_SYNC", 2048, NULL, 1, NULL, 0);
}

uint32_t rtc_now_ms_of_day() {
    if (!s_rtc_synced) return millis();  // Fallback: Uptime
    uint32_t elapsed = millis() - s_ref_millis;
    return (s_ref_ms_of_day + elapsed) % 86400000UL;
}


String rtc_now_str() {
    if (!s_rtc_synced) {
        // Relative Zeit seit Start
        uint32_t ms = millis();
        uint32_t h  = ms / 3600000;
        uint32_t m  = (ms % 3600000) / 60000;
        uint32_t s  = (ms % 60000) / 1000;
        uint32_t mm = ms % 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "+%02lu:%02lu:%02lu.%03lu", h, m, s, mm);
        return String(buf);
    }
    uint32_t t  = rtc_now_ms_of_day();
    uint32_t h  = t / 3600000;
    uint32_t m  = (t % 3600000) / 60000;
    uint32_t s  = (t % 60000) / 1000;
    uint32_t mm = t % 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%03lu", h, m, s, mm);
    return String(buf);
}