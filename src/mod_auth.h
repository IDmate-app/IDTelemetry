#pragma once
#include <Arduino.h>

class AsyncWebServerRequest;

// ============================================================
//  mod_auth - Anmeldung an der Weboberflaeche (1.7.8, L-028)
// ============================================================
//  Anmelden mit dem AP-Passwort (POST /api/login) gibt ein Sitzungs-Cookie,
//  30 min gueltig, verlaengert sich bei jeder Nutzung. Geschuetzt: Geheimnisse
//  in /api/config, Speichern der Einstellungen, alles was auf den CAN-Bus
//  sendet, Kopplungsfenster. Ohne AP-Passwort (offenes Netz) ist alles frei.
//
//  Sperr-Ausweg: Boot-Taste (GPIO0) im laufenden Betrieb 5 s halten → 3 min
//  ohne Passwort (z. B. um ein vergessenes Passwort neu zu setzen). NICHT
//  beim Einschalten halten: dann startet der ESP32 in den Flash-Modus.

void auth_init();
void auth_tick();                                   // aus loop()
bool auth_ok(AsyncWebServerRequest* r);             // angemeldet, offen oder Fenster
bool auth_required();                               // AP-Passwort gesetzt?
uint32_t auth_unlock_left_s();                      // Rest des Fensters (0 = zu)
// pw pruefen; bei Erfolg Cookie-Wert (Token) in out (>= 33 Zeichen)
bool auth_login(const char* pw, char* out, size_t out_sz);
void auth_logout(AsyncWebServerRequest* r);
void auth_deny(AsyncWebServerRequest* r);           // 401 {"ok":false,"err":"auth"}
