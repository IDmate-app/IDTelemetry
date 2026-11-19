#include "config.h"
#include "mod_auth.h"
#include "mod_config.h"
#include "mod_logs.h"
#include <ESPAsyncWebServer.h>
#include <esp_random.h>

#define AUTH_BTN_PIN        0          // Boot-Taste
#define AUTH_BTN_HOLD_MS    5000UL
#define AUTH_UNLOCK_MS      180000UL   // 3 min
#define AUTH_SESSION_MS     1800000UL  // 30 min, gleitend
#define AUTH_SESSIONS       4
#define AUTH_COOKIE         "idt_s"

struct Session { char tok[33]; uint32_t last_ms; bool used; };
static Session  s_sess[AUTH_SESSIONS];
static uint32_t s_unlock_until = 0;     // millis(), 0 = zu
static uint32_t s_btn_down_ms  = 0;
static bool     s_btn_fired    = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void auth_init() {
    pinMode(AUTH_BTN_PIN, INPUT_PULLUP);
    memset(s_sess, 0, sizeof(s_sess));
}

static bool unlocked() {
    return s_unlock_until != 0 && (int32_t)(s_unlock_until - millis()) > 0;
}

uint32_t auth_unlock_left_s() {
    if (!unlocked()) return 0;
    return (s_unlock_until - millis() + 999) / 1000;
}

bool auth_required() {
    const char* ap = cfg_ap_pass();
    return ap && ap[0];
}

void auth_tick() {
    uint32_t now = millis();
    bool down = digitalRead(AUTH_BTN_PIN) == LOW;
    if (!down) { s_btn_down_ms = 0; s_btn_fired = false; }
    else if (s_btn_down_ms == 0) s_btn_down_ms = now ? now : 1;
    else if (!s_btn_fired && now - s_btn_down_ms >= AUTH_BTN_HOLD_MS) {
        s_btn_fired = true;
        s_unlock_until = (now + AUTH_UNLOCK_MS) | 1;
        syslog("WEB", "Boot-Taste 5 s: Weboberflaeche 3 min ohne Passwort offen");
    }
    if (s_unlock_until && !unlocked()) {
        s_unlock_until = 0;
        syslog("WEB", "Fenster ohne Passwort zu");
    }
}

static bool cookie_token(AsyncWebServerRequest* r, char* out, size_t sz) {
    if (!r->hasHeader("Cookie")) return false;
    String c = r->getHeader("Cookie")->value();
    int i = c.indexOf(AUTH_COOKIE "=");
    while (i > 0 && c[i - 1] != ' ' && c[i - 1] != ';') i = c.indexOf(AUTH_COOKIE "=", i + 1);
    if (i < 0) return false;
    i += strlen(AUTH_COOKIE) + 1;
    int e = c.indexOf(';', i);
    String v = e < 0 ? c.substring(i) : c.substring(i, e);
    v.trim();
    if (v.length() != 32) return false;
    strncpy(out, v.c_str(), sz - 1); out[sz - 1] = '\0';
    return true;
}

bool auth_ok(AsyncWebServerRequest* r) {
    if (!auth_required() || unlocked()) return true;
    char tok[33];
    if (!cookie_token(r, tok, sizeof(tok))) return false;
    bool ok = false;
    uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    for (auto& s : s_sess) {
        if (!s.used) continue;
        if (now - s.last_ms > AUTH_SESSION_MS) { s.used = false; continue; }
        if (strcmp(s.tok, tok) == 0) { s.last_ms = now; ok = true; }
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool auth_login(const char* pw, char* out, size_t out_sz) {
    if (out_sz < 33) return false;
    bool good = !auth_required() || unlocked() || (pw && strcmp(pw, cfg_ap_pass()) == 0);
    if (!good) {
        syslog("WEB", "Anmeldung abgelehnt (falsches Passwort)");
        delay(800);   // Raten bremsen
        return false;
    }
    for (int k = 0; k < 4; k++) snprintf(out + k * 8, 9, "%08lx", (unsigned long)esp_random());
    uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    int slot = 0;                               // freier oder aeltester Platz
    for (int k = 0; k < AUTH_SESSIONS; k++) {
        if (!s_sess[k].used) { slot = k; break; }
        if (s_sess[k].last_ms < s_sess[slot].last_ms) slot = k;
    }
    strncpy(s_sess[slot].tok, out, 32); s_sess[slot].tok[32] = '\0';
    s_sess[slot].last_ms = now;
    s_sess[slot].used = true;
    portEXIT_CRITICAL(&s_mux);
    syslog("WEB", "Angemeldet");
    return true;
}

void auth_logout(AsyncWebServerRequest* r) {
    char tok[33];
    if (!cookie_token(r, tok, sizeof(tok))) return;
    portENTER_CRITICAL(&s_mux);
    for (auto& s : s_sess) if (s.used && strcmp(s.tok, tok) == 0) s.used = false;
    portEXIT_CRITICAL(&s_mux);
}

void auth_deny(AsyncWebServerRequest* r) {
    r->send(401, "application/json", "{\"ok\":false,\"err\":\"auth\"}");
}
