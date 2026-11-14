#pragma once
#include <stdint.h>

// ============================================================
//  mod_live — Live-Telegramme ueber Bluetooth fuer Anzeigegeraete
//
//  Ein Anzeigegeraet im Auto (z. B. die IDmate-App fuer Android Auto)
//  bekommt den Zustand direkt vom Stick, ohne Umweg ueber LTE und Server.
//
//  AB WERK AUS (Einstellungen -> "Live-Daten ueber Bluetooth"). Solange es
//  aus ist, gibt es den Dienst nicht.
//
//  GATT (zusaetzlich zur ELM327-Emulation FFE0 fuer ABRP, die offen bleibt):
//    Dienst      1d7e0001-6d61-7465-2d6c-697665000000
//    Telegramm   1d7e0002-...   Notify + Read: ein CBOR-Telegramm je Nachricht
//
//  Die Telegramme enthalten Position und Fahrzustand. Deshalb gehen sie nur
//  an ein GEKOPPELTES Geraet: Kopplung mit sechsstelligem Code, Verbindung
//  verschluesselt UND authentifiziert. Geprueft wird je Verbindung vor jedem
//  Senden - ein Geraet, das nur "Just Works" gekoppelt hat, bekommt nichts.
//
//  KOPPLUNGSFENSTER: Einen festen Code koennte jemand in Funkreichweite mit
//  rund 20 Versuchen ermitteln (das Verfahren verraet je Fehlversuch ein Bit,
//  wenn der Code derselbe bleibt). Deshalb:
//    - Der Code entsteht erst, wenn in den Einstellungen "Geraet koppeln"
//      gedrueckt wird, gilt LIVE_PAIR_WINDOW_S Sekunden und fuer EINEN Versuch.
//    - Jeder Kopplungsversuch - gelungen oder nicht - verbraucht ihn.
//    - Ausserhalb des Fensters gilt ein geheimer Zufallscode, der nirgends
//      angezeigt und nach jedem Versuch ersetzt wird.
//  Schon gekoppelte Geraete verbinden sich ohne Code wieder.
//
//  Takt: 1 s in Fahrt, 30 s im Stand. Der Stick bleibt das eine OBD-
//  Hauptgeraet: mod_live fragt den Bus nicht selbst ab, es liest den
//  Telemetrie-Cache, den der Stick ohnehin fuellt.
// ============================================================

#define LIVE_SERVICE_UUID "1d7e0001-6d61-7465-2d6c-697665000000"
#define LIVE_CHAR_UUID    "1d7e0002-6d61-7465-2d6c-697665000000"

#define LIVE_PAIR_WINDOW_S 180

void live_init();            // nach ble_scan_init(), vor elm327_init(); tut nichts, wenn ausgeschaltet
// {"on":true,"subs":1,"sent":123,"skipped":0,"no_time":0,"pair_s":0} fuer /status und die Fehlersuche
void live_status_str(char* buf, int size);

uint32_t live_pair_open();           // Kopplungsfenster oeffnen: der neue Code, 0 wenn der Dienst aus ist
uint32_t live_pair_seconds_left();   // 0 = Fenster zu
// Aus den Server-Callbacks: ein Kopplungsversuch ist beendet (encrypted = gelungen).
void live_auth_done(bool encrypted);
