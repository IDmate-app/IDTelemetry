#pragma once
// ============================================================
//  live_cbor — Telegramme fuer Anzeigegeraete als CBOR (RFC 8949)
//
//  Dasselbe Telegramm, das IDmate ueber seine Lese-API als JSON liefert
//  (IDMate docs/read-api.md), nur binaer:
//    {"v":1,"src":"stick","ts":"2026-11-12T14:03:21Z","seq":4711,
//     "type":"status","data":{"soc":71.5,"speed":87.0,...}}
//  Ein Feld ohne gueltigen Wert fehlt - nie null, nie 0.
//
//  Ohne Arduino/NimBLE, damit der Aufbau auf dem PC pruefbar ist
//  (tools/live_cbor_test.cpp). Nur der Ausschnitt von CBOR, den die
//  Telegramme brauchen: Map, Text, positive Ganzzahl, Float32/64, Bool.
// ============================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

struct LiveCbor {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    bool     overflow;

    LiveCbor(uint8_t* b, size_t c) : buf(b), cap(c), len(0), overflow(false) {}

    void byte(uint8_t b) {
        if (len < cap) buf[len++] = b; else overflow = true;
    }
    void head(uint8_t major, uint32_t n) {
        if (n < 24)          { byte((major << 5) | n); }
        else if (n < 0x100)  { byte((major << 5) | 24); byte(n); }
        else if (n < 0x10000){ byte((major << 5) | 25); byte(n >> 8); byte(n); }
        else                 { byte((major << 5) | 26); byte(n >> 24); byte(n >> 16); byte(n >> 8); byte(n); }
    }
    void map(uint32_t pairs)   { head(5, pairs); }
    void uint(uint32_t n)      { head(0, n); }
    void text(const char* s)   { size_t n = strlen(s); head(3, (uint32_t)n); for (size_t i = 0; i < n; i++) byte((uint8_t)s[i]); }
    void boolean(bool b)       { byte(b ? 0xF5 : 0xF4); }
    void f32(float f)          { uint32_t u; memcpy(&u, &f, 4); byte(0xFA); byte(u >> 24); byte(u >> 16); byte(u >> 8); byte(u); }
    void f64(double d)         { uint64_t u; memcpy(&u, &d, 8); byte(0xFB); for (int i = 7; i >= 0; i--) byte((uint8_t)(u >> (i * 8))); }
};

// Werte eines Telegramms. has_* = Wert gueltig und frisch.
struct LiveValues {
    bool has_soc, has_range, has_power, has_speed, has_odometer;
    bool has_charging, has_dc, has_t_ext, has_t_batt, has_pos;
    float soc, range, power, speed, odometer, t_ext, t_batt;
    bool  charging, dc;
    double lat, lon;
};

static inline void live_iso8601(uint32_t unix_s, char out[21]) {
    time_t t = (time_t)unix_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static inline void live_envelope(LiveCbor& c, uint32_t unix_s, uint32_t seq, const char* type) {
    char ts[21];
    live_iso8601(unix_s, ts);
    c.map(6);
    c.text("v");    c.uint(1);
    c.text("src");  c.text("stick");
    c.text("ts");   c.text(ts);
    c.text("seq");  c.uint(seq);
    c.text("type"); c.text(type);
    c.text("data");
}

// Zustand ohne Position. Rueckgabe: Laenge oder 0 (Puffer zu klein / kein Feld).
static inline size_t live_encode_status(const LiveValues& v, uint32_t unix_s, uint32_t seq, uint8_t* out, size_t cap) {
    uint32_t n = v.has_soc + v.has_range + v.has_power + v.has_speed + v.has_odometer + v.has_charging
               + v.has_dc + v.has_t_ext + v.has_t_batt;
    if (n == 0) return 0;
    LiveCbor c(out, cap);
    live_envelope(c, unix_s, seq, "status");
    c.map(n);
    if (v.has_soc)      { c.text("soc");      c.f32(v.soc); }
    if (v.has_range)    { c.text("range");    c.f32(v.range); }
    if (v.has_power)    { c.text("power");    c.f32(v.power); }
    if (v.has_speed)    { c.text("speed");    c.f32(v.speed); }
    if (v.has_odometer) { c.text("odometer"); c.f32(v.odometer); }
    if (v.has_charging) { c.text("charging"); c.boolean(v.charging); }
    if (v.has_dc)       { c.text("dc");       c.boolean(v.dc); }
    if (v.has_t_ext)    { c.text("t_ext");    c.f32(v.t_ext); }
    if (v.has_t_batt)   { c.text("t_batt");   c.f32(v.t_batt); }
    return c.overflow ? 0 : c.len;
}

// Position (Float64: Float32 loest bei 50 Grad nur ~0,5 m auf, das genuegt nicht fuer jede Karte).
static inline size_t live_encode_position(const LiveValues& v, uint32_t unix_s, uint32_t seq, uint8_t* out, size_t cap) {
    if (!v.has_pos) return 0;
    LiveCbor c(out, cap);
    live_envelope(c, unix_s, seq, "position");
    c.map(2);
    c.text("lat"); c.f64(v.lat);
    c.text("lon"); c.f64(v.lon);
    return c.overflow ? 0 : c.len;
}

// Faehigkeiten: je Feld, das gerade lieferbar ist, das Intervall in Sekunden.
static inline size_t live_encode_caps(const LiveValues& v, uint32_t unix_s, uint32_t seq, uint32_t interval_s,
                                      uint8_t* out, size_t cap) {
    uint32_t n = v.has_soc + v.has_range + v.has_power + v.has_speed + v.has_odometer + v.has_charging
               + v.has_dc + v.has_t_ext + v.has_t_batt;
    LiveCbor c(out, cap);
    live_envelope(c, unix_s, seq, "caps");
    c.map(v.has_pos ? 2 : 1);
    c.text("status");
    c.map(n);
    if (v.has_soc)      { c.text("soc");      c.uint(interval_s); }
    if (v.has_range)    { c.text("range");    c.uint(interval_s); }
    if (v.has_power)    { c.text("power");    c.uint(interval_s); }
    if (v.has_speed)    { c.text("speed");    c.uint(interval_s); }
    if (v.has_odometer) { c.text("odometer"); c.uint(interval_s); }
    if (v.has_charging) { c.text("charging"); c.uint(interval_s); }
    if (v.has_dc)       { c.text("dc");       c.uint(interval_s); }
    if (v.has_t_ext)    { c.text("t_ext");    c.uint(interval_s); }
    if (v.has_t_batt)   { c.text("t_batt");   c.uint(interval_s); }
    if (v.has_pos) {
        c.text("position");
        c.map(2);
        c.text("lat"); c.uint(interval_s);
        c.text("lon"); c.uint(interval_s);
    }
    return c.overflow ? 0 : c.len;
}
