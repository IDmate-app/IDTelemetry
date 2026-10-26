// TINY_GSM_MODEM_SIM7080 wird ausschließlich über platformio.ini build_flags gesetzt.
#include <TinyGsmClient.h>
#include <Preferences.h>

#include "mod_modem.h"
#include "secrets.h"
#include "mod_sleep.h"
#include "mod_telemetry.h"
#include "mod_mqtt.h"
#include "shared.h"
#include "config.h"
#include "mod_config.h"
#include "mod_gps_ext.h"
#include "mod_rtc.h"
#include "mod_logs.h"
#include "mod_pmu.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <math.h>

// ============================================================
//  mod_modem - SIM7080G Modem (LTE + GPS)
//
//  Eine FreeRTOS-Task verwaltet alles sequenziell:
//    1. Traccar-Tick (alle 60 s bei GPS-Fix → HTTPS GET)
//    2. GPS abfragen (AT+CGNSINF via TinyGSM) — alle 5 s
//    3. Modem-Status (Signal, Anbieter, SIM) — alle ~30 s
//
//  A-GPS: RTC-Zeit + letzte Position beim Start → schnellerer Fix
// ============================================================

// ---- Modem-Objekte ----------------------------------------

static HardwareSerial  s_serial(1);         // UART1 — exklusiv
static TinyGsm         s_modem(s_serial);

// ---- RTC-persistenter Reboot-Limiter (überlebt esp_restart, nicht Power-Loss) ----
RTC_DATA_ATTR static uint8_t  s_wd_reboot_count       = 0;
RTC_DATA_ATTR static uint32_t s_wd_first_reboot_epoch = 0;

// ---- Zustand ----------------------------------------------

static volatile bool     s_connected   = false;
static bool              s_had_lte     = false;  // true nach erster erfolgreicher GPRS-Verbindung
static char              s_operator[48] = "";
static volatile int8_t   s_sig_quality  = -1;  // CSQ 0-31, 99=kein Signal, -1=unbekannt
static volatile uint16_t s_plmn_num     = 0;   // aktuelles PLMN als Zahl (26201 Telekom, 26203 o2)
static volatile bool     s_sim_ok       = false;

// ---- GPS-Details (vom Task geschrieben, per Getter lesbar) ----
static volatile float  s_gps_alt      = 0;
static volatile float  s_gps_speed    = 0;
static volatile float  s_gps_hdop     = 0;
static volatile int    s_gps_vsat     = 0;
static volatile int    s_gps_usat     = 0;
static volatile bool   s_gps_enabled  = false;
static volatile bool   s_gps_has_fix  = false;

// ---- Letzter Fix für Kursberechnung ----
static double s_prev_fix_lat = 0.0;
static double s_prev_fix_lon = 0.0;
static bool   s_prev_fix_ok  = false;

// ---- Interne Linkage für mod_traccar ----------------------

TinyGsm& modem_get() { return s_modem; }
HardwareSerial& modem_serial() { return s_serial; }

// ---- Modem Power-On ---------------------------------------



// PWRKEY-Impuls: genau wie im LilyGo-Beispiel (AllFunction/modem.cpp)
static void modem_pwrkey_pulse() {
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
}

// Hardware Power-Off via PMU — kein AT erforderlich, kein UART nötig
static void modem_pmu_poweroff() {
    pmu_set_modem_power(false);  // DC3 aus → Modem stromlos
    delay(200);
}



static bool modem_ntp_sync();  // Forward-Declaration

static TaskHandle_t s_modem_task_handle = nullptr;
static volatile bool s_task_paused = false;  // "at stop" → Modem-Task pausiert

// ---- A-GPS: RTC-Zeit + Warm Start → schnellerer Fix ----

// GPS aktivieren inkl. Multi-GNSS Konfiguration.
// +CGNSMOD muss NACH disableGPS / VOR enableGPS gesetzt werden.
// Nach erster LTE-Verbindung: Hot Start (AT+CGNSHOT) — nutzt gespeicherte letzte
// Position + Almanach + Modem-Zeit (automatisch via LTE synchronisiert).
static bool gps_enable_with_config() {
    if (!cfg_mod_gps()) {
        syslog("GPS", "Deaktiviert (Modul-Config = 0)");
        return false;
    }
    s_modem.disableGPS();
    delay(300);
    // Multi-GNSS nur beim ersten Start setzen — danach bleibt Config im NVRAM.
    // Wiederholtes CGNSMOD invalidiert Ephemeris-Cache → erzwingt Cold Start.
    static bool s_gnss_configured = false;
    if (!s_gnss_configured) {
        s_modem.sendAT("+CGNSMOD=1,1,0,1");  // GPS + GLONASS + BeiDou
        s_modem.waitResponse(1000);
        s_gnss_configured = true;
    }
    bool ok = s_modem.enableGPS();          // ZUERST GPS einschalten
    if (ok && s_had_lte) {
        delay(200);                          // GPS-Engine braucht Moment zum Starten
        s_modem.sendAT("+CGNSHOT");         // DANN Hot Start (NVRAM-Pos + LTE-Zeit)
        s_modem.waitResponse(1000);
        syslog("GPS", "Hot Start (NVRAM + LTE-Zeit)");
    }
    return ok;
}




// ---- FreeRTOS Task (State Machine) --------------------



enum ModemState {

    STATE_CHECK_SIM,

    STATE_WAIT_FOR_NETWORK,

    STATE_RUNNING

};

// Hilfsfunktion: AT-Befehl senden, auf OK warten, Antwort loggen.
// Gibt true bei OK (rc=1) zurueck.
static bool at_ok(const char* cmd, long timeout_ms = 5000L) {
    s_modem.sendAT(cmd);
    String resp = "";
    int rc = s_modem.waitResponse(timeout_ms, resp);
    if (rc != 1) {
        char msg[128];
        resp.trim();
        snprintf(msg, sizeof(msg), "AT%s → ERROR (%s)", cmd, resp.c_str());
        syslog("MODEM", msg);
    }
    return rc == 1;
}

// ---- Europaeische LTE-M Operator-Tabelle ----
// Nur noch fuer Namens-Lookup nach Auto-Modus-Verbindung.
// Netzwahl: Green-List (SPIFFS) → Auto-Modus (COPS=0).
struct PlmnEntry { const char* plmn; const char* name; };
static const PlmnEntry PLMN_TABLE[] = {
    // Things Mobile LTE-M Roaming-Partner, sortiert nach Naehe zu Deutschland
    // Deutschland — O2 LTE-M+NB-IoT, T-Mobile LTE-M
    { "26203", "o2 DE"          },
    { "26201", "T-Mobile DE"    },
    // Luxemburg — POST LTE-M
    { "27001", "POST LU"        },
    // Tschechien — O2 LTE-M
    { "23002", "O2 CZ"          },
    // Niederlande — T-Mobile LTE-M, Vodafone LTE-M
    { "20416", "T-Mobile NL"    },
    { "20404", "Vodafone NL"    },
    // Belgien — Orange LTE-M, Base LTE-M
    { "20610", "Orange BE"      },
    { "20620", "Base BE"        },
    // Oesterreich — T-Mobile LTE-M
    { "23203", "T-Mobile AT"    },
    // Daenemark — TDC LTE-M, 3DK LTE-M, Telia LTE-M
    { "23801", "TDC DK"         },
    { "23806", "3 DK"           },
    { "23820", "Telia DK"       },
    // Schweiz — Swisscom LTE-M, Sunrise LTE-M
    { "22801", "Swisscom"       },
    { "22802", "Sunrise CH"     },
    // Slowakei — Orange LTE-M
    { "23101", "Orange SK"      },
    // Slowenien — A1 LTE-M
    { "29340", "A1 SI"          },
    // Frankreich — Orange LTE-M, Bouygues LTE-M
    { "20801", "Orange FR"      },
    { "20820", "Bouygues FR"    },
    // UK — Vodafone LTE-M
    { "23415", "Vodafone UK"    },
    // Ungarn — T-Mobile LTE-M
    { "21630", "T-Mobile HU"    },
    // Schweden — Telia LTE-M, Tele2 LTE-M, Telenor LTE-M
    { "24001", "Telia SE"       },
    { "24007", "Tele2 SE"       },
    { "24008", "Telenor SE"     },
    // Norwegen — Telenor LTE-M, Telia LTE-M
    { "24201", "Telenor NO"     },
    { "24202", "Telia NO"       },
    // Estland — Telia LTE-M, Elisa LTE-M
    { "24801", "Telia EE"       },
    { "24802", "Elisa EE"       },
    // Lettland — LMT LTE-M
    { "24701", "LMT LV"         },
    // Rumaenien — Vodafone LTE-M, Orange LTE-M
    { "22601", "Vodafone RO"    },
    { "22610", "Orange RO"      },
    // Finnland — Telia LTE-M, Elisa LTE-M
    { "24491", "Telia FI"       },
    { "24405", "Elisa FI"       },
    // Spanien — Movistar LTE-M
    { "21407", "Movistar ES"    },
    // Tuerkei — Turkcell LTE-M
    { "28601", "Turkcell TR"    },
    // Portugal — NOS LTE-M
    { "26803", "NOS PT"         },
    // Island — Siminn LTE-M
    { "27401", "Siminn IS"      },
};
static const int PLMN_COUNT = sizeof(PLMN_TABLE) / sizeof(PLMN_TABLE[0]);

// ---- PLMN-Hilfsfunktion: ist PLMN in PLMN_TABLE (Whitelist)? ----
static bool is_allowed_plmn(const char* plmn) {
    for (int i = 0; i < PLMN_COUNT; i++) {
        if (strcmp(PLMN_TABLE[i].plmn, plmn) == 0) return true;
    }
    return false;
}

// SIM PIN pruefen und ggf. eingeben. Gibt true zurueck wenn SIM READY.
static bool ensure_sim_ready() {
    char syslog_msg[128];
    String cpin = "";
    s_modem.sendAT("+CPIN?");
    s_modem.waitResponse(5000L, cpin);
    cpin.trim();

    if (cpin.indexOf("READY") >= 0) return true;

    if (cpin.indexOf("SIM PIN") >= 0) {
        const char* pin = cfg_sim_pin();
        if (strlen(pin) == 0) {
            syslog("MODEM", "SIM braucht PIN, aber keiner konfiguriert!");
            return false;
        }
        snprintf(syslog_msg, sizeof(syslog_msg), "+CPIN=%s", pin);
        syslog("MODEM", "PIN eingeben...");
        s_modem.sendAT(syslog_msg);
        if (s_modem.waitResponse(10000L) != 1) {
            syslog("MODEM", "PIN abgelehnt!");
            return false;
        }
        syslog("MODEM", "PIN akzeptiert");

        // SIM braucht nach PIN+CFUN bis zu 15s — mehrfach pruefen
        for (int retry = 0; retry < 6; retry++) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            cpin = "";
            s_modem.sendAT("+CPIN?");
            s_modem.waitResponse(5000L, cpin);
            cpin.trim();
            if (cpin.indexOf("READY") >= 0) return true;
            snprintf(syslog_msg, sizeof(syslog_msg),
                     "SIM nach PIN: %s (Versuch %d/6)", cpin.c_str(), retry + 1);
            syslog("MODEM", syslog_msg);
        }

        syslog("MODEM", "SIM nach PIN nicht READY");
        return false;
    }

    snprintf(syslog_msg, sizeof(syslog_msg), "SIM-Status unerwartet: %s", cpin.c_str());
    syslog("MODEM", syslog_msg);
    return false;
}

// Radio konfigurieren — schlanke Variante wie LilyGo-Beispiel.
// Kein CFUN=0/1 Zyklus (nur nötig für GPS↔LTE Toggle).
// Setzt LTE-M, APN und Registrierungs-URCs.
static bool configure_radio() {
    syslog("MODEM", "Radio konfigurieren...");

    // UART-Buffer leeren — nach PIN-Eingabe kommen URCs die den Parser stoeren
    while (s_serial.available()) s_serial.read();

    at_ok("+CNMP=38");                   // LTE only
    at_ok("+CMNB=1");                    // CAT-M only (kein NB-IoT)
    { char cmd[80]; snprintf(cmd, sizeof(cmd), "+CGDCONT=1,\"IP\",\"%s\"", cfg_apn()); at_ok(cmd); }
    { char cmd[80]; snprintf(cmd, sizeof(cmd), "+CNCFG=0,1,\"%s\"",       cfg_apn()); at_ok(cmd); }

    at_ok("+CEREG=2");                   // Netz-Registrierung mit Location-Info
    at_ok("+CPSMS=0");                   // Power Save Mode aus
    at_ok("+CEDRXS=0");                  // eDRX aus

    syslog("MODEM", "Radio konfiguriert: LTE CAT-M");
    return true;
}

// Warte auf Registrierung (max wait_s Sekunden). Gibt REG_OK_HOME/ROAMING zurueck oder letzten Status.
static RegStatus wait_for_reg(uint32_t wait_s) {
    char syslog_msg[128];
    RegStatus  reg_s     = REG_UNKNOWN;
    uint32_t   log_ms    = millis();
    uint32_t   denied_cnt = 0;

    for (uint32_t s = 0; s < wait_s && !g_shutdown; s++) {
        reg_s = s_modem.getRegistrationStatus();
        if (reg_s == REG_OK_HOME || reg_s == REG_OK_ROAMING) return reg_s;

        // RegStatus=3 (denied): Netz lehnt SIM ab → nach 10s abbrechen
        if (reg_s == REG_DENIED) {
            if (++denied_cnt >= 10) {
                // Aktuelles PLMN abfragen und loggen
                s_modem.sendAT("+COPS=3,2");
                s_modem.waitResponse(3000L);
                String dresp = "";
                s_modem.sendAT("+COPS?");
                s_modem.waitResponse(3000L, dresp);
                int dq1 = dresp.indexOf('"');
                int dq2 = (dq1 >= 0) ? dresp.indexOf('"', dq1 + 1) : -1;
                if (dq1 >= 0 && dq2 > dq1) {
                    String dplmn = dresp.substring(dq1 + 1, dq2);
                    snprintf(syslog_msg, sizeof(syslog_msg), "Netz verweigert (denied 10x) · PLMN %s", dplmn.c_str());
                } else {
                    snprintf(syslog_msg, sizeof(syslog_msg), "Netz verweigert (denied 10x) · PLMN unbekannt");
                }
                syslog("MODEM", syslog_msg);
                return REG_DENIED;
            }
        } else {
            denied_cnt = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        if (millis() - log_ms >= 15000UL) {
            snprintf(syslog_msg, sizeof(syslog_msg),
                     "Netzsuche laeuft... (%lus) RegStatus=%d", (unsigned long)s, (int)reg_s);
            syslog("MODEM", syslog_msg);
            log_ms = millis();
        }
    }
    return reg_s;
}

// ── AT+CPSI? — UE-System-Info (nicht-destruktiv, liest nur aktuelle Zelle) ──
// Erwartetes Format LTE CAT-M:
//   +CPSI: LTE CAT-M1,Online,MCC-MNC,TAC,SCellID,PCellID,EUTRAN-BANDx,EARFCN,
//          dlbw,ulbw,RSRQ,RSRP,RSSI,RSSNR
// Bei NO SERVICE: kuerzer ("+CPSI: NO SERVICE,Online" o.ae.).
// Schreibt eine kompakte Log-Zeile via syslog (oder Serial bei verbose=true).
// Reine Read-Operation, kein Side-Effect auf Registrierung.
static void cpsi_query_log(bool verbose) {
    s_modem.sendAT("+CPSI?");
    String resp;
    if (s_modem.waitResponse(2000L, resp) != 1) {
        if (verbose) Serial.println("[LTE] CPSI: keine Antwort");
        return;
    }
    int p = resp.indexOf("+CPSI:");
    if (p < 0) {
        if (verbose) Serial.printf("[LTE] CPSI: unerwartet: %s\n", resp.c_str());
        return;
    }
    String line = resp.substring(p + 6);  // nach "+CPSI:"
    // bis Zeilenende oder OK
    int eol = line.indexOf('\r');
    if (eol < 0) eol = line.indexOf('\n');
    if (eol > 0) line = line.substring(0, eol);
    line.trim();

    if (verbose) Serial.printf("[LTE] CPSI raw: %s\n", line.c_str());

    // Felder kommagetrennt zaehlen
    int field_count = 1;
    for (int i = 0; i < (int)line.length(); i++) if (line[i] == ',') field_count++;

    if (field_count < 4) {
        // "NO SERVICE" oder aehnlich
        char m[64];
        snprintf(m, sizeof(m), "CPSI: %s", line.c_str());
        if (verbose) Serial.println(m); else syslog("LTE", m);
        return;
    }

    // Felder extrahieren
    auto field = [&](int n) -> String {
        int start = 0, idx = 0;
        for (int i = 0; i < (int)line.length(); i++) {
            if (line[i] == ',') {
                if (idx == n) return line.substring(start, i);
                idx++;
                start = i + 1;
            }
        }
        if (idx == n) return line.substring(start);
        return "";
    };

    String sysmode = field(0);    // "LTE CAT-M1" / "LTE NB-IOT"
    String plmn    = field(2);    // "262-01"
    String band    = field(6);    // "EUTRAN-BAND8"
    String s_rsrq  = field(10);
    String s_rsrp  = field(11);
    String s_rssi  = field(12);
    String s_snr   = field(13);

    char m[128];
    snprintf(m, sizeof(m),
        "%s · %s · %s · RSRP=%sdBm RSRQ=%sdB RSSI=%sdBm SNR=%s",
        sysmode.c_str(), plmn.c_str(), band.c_str(),
        s_rsrp.c_str(), s_rsrq.c_str(), s_rssi.c_str(), s_snr.c_str());

    if (verbose) Serial.println(m); else syslog("LTE", m);
}

// Aktuelles PLMN numerisch abfragen
static void get_current_plmn(char* plmn_out, size_t size) {
    plmn_out[0] = '\0';
    s_modem.sendAT("+COPS=3,2");
    s_modem.waitResponse(3000L);
    String resp = "";
    s_modem.sendAT("+COPS?");
    s_modem.waitResponse(3000L, resp);
    int q1 = resp.indexOf('"');
    int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) {
        String plmn = resp.substring(q1 + 1, q2);
        strncpy(plmn_out, plmn.c_str(), size - 1);
        plmn_out[size - 1] = '\0';
    }
}

// Operator-Name ermitteln und loggen (nach erfolgreicher Registrierung)
static void lookup_operator(char* operator_out, size_t op_size) {
    char syslog_msg[128];

    String op = s_modem.getOperator();
    op.trim();
    if (op.length() > 0) {
        strncpy(operator_out, op.c_str(), op_size - 1);
        operator_out[op_size - 1] = '\0';
    } else {
        strncpy(operator_out, "Auto", op_size - 1);
        operator_out[op_size - 1] = '\0';
    }

    char plmn[8] = "";
    get_current_plmn(plmn, sizeof(plmn));
    if (plmn[0]) {
        snprintf(syslog_msg, sizeof(syslog_msg), "PLMN: %s", plmn);
        syslog("MODEM", syslog_msg);
        // Numerischen PLMN fuer Telemetrie ablegen
        s_plmn_num = (uint16_t)atoi(plmn);
        // Name aus PLMN_TABLE nachschlagen
        for (int j = 0; j < PLMN_COUNT; j++) {
            if (strcmp(PLMN_TABLE[j].plmn, plmn) == 0) {
                strncpy(operator_out, PLMN_TABLE[j].name, op_size - 1);
                operator_out[op_size - 1] = '\0';
                break;
            }
        }
    } else {
        s_plmn_num = 0;
    }
    // Format zurueck auf Langname
    s_modem.sendAT("+COPS=3,0");
    s_modem.waitResponse(3000L);
}

// FIXES B.12: Zeitpunkt des letzten vollen COPS=?-Scans (deregistriert + bis 3 min).
static uint32_t s_last_plmn_scan_ms = 0;

// PLMN-Scan: verfuegbare Netze scannen, gegen PLMN_TABLE filtern, bestes verbinden.
// ACHTUNG: AT+COPS=? deregistriert das Modem vom aktuellen Netz!
// Gibt true bei erfolgreicher Registrierung zurueck.
static bool plmn_scan_and_register(char* operator_out, size_t op_size) {
    char syslog_msg[128];

    // FIXES B.12: höchstens 1×/h einen vollen COPS=?-Scan — sonst scannt das
    // Modem in Funkloch-Lage mit VBUS (Tiefgarage/Laden über Nacht) stundenlang
    // im Kreis. Innerhalb der Stunde direkt den günstigen Auto-Modus versuchen.
    if (s_last_plmn_scan_ms != 0 && (millis() - s_last_plmn_scan_ms) < 3600000UL) {
        syslog("MODEM", "PLMN-Scan übersprungen (<1h) · Auto-Modus 60s");
        s_modem.sendAT("+COPS=0");
        s_modem.waitResponse(10000L);
        RegStatus reg = wait_for_reg(60);
        if (reg == REG_OK_HOME || reg == REG_OK_ROAMING) {
            lookup_operator(operator_out, op_size);
            return true;
        }
        return false;
    }
    s_last_plmn_scan_ms = millis();

    syslog("MODEM", "PLMN-Scan laeuft (bis 3 Min)...");

    s_modem.sendAT("+COPS=?");
    String cops = "";
    int r = s_modem.waitResponse(180000L, cops);

    // Kandidaten sammeln (max 5, auf dem Stack)
    struct Candidate { char plmn[8]; int stat; };
    Candidate cands[5];
    int cand_count = 0;

    if (r == 1 && cops.length() > 5) {
        // Format: (stat,"longname","shortname","plmn",act)
        int pos = 0;
        while (pos < (int)cops.length() && cand_count < 5) {
            int ns = cops.indexOf('(', pos);
            if (ns < 0) break;
            int ne = cops.indexOf(')', ns);
            if (ne < 0) break;
            String entry = cops.substring(ns + 1, ne);

            // stat (1=available, 2=current, 3=forbidden)
            int stat_e = entry.substring(0, entry.indexOf(',')).toInt();

            // act (letztes Feld) — nur LTE-M (7) akzeptieren
            int lc = entry.lastIndexOf(',');
            int act = (lc >= 0) ? entry.substring(lc + 1).toInt() : 0;

            // PLMN extrahieren (3. Quoted-String: "plmn")
            int q = -1;
            for (int skip = 0; skip < 4; skip++) {  // 4 Quotes überspringen = 2 Strings
                q = entry.indexOf('"', q + 1);
                if (q < 0) break;
            }
            int q5 = (q >= 0) ? entry.indexOf('"', q + 1) : -1;  // Start des 3. Strings
            int q6 = (q5 >= 0) ? entry.indexOf('"', q5 + 1) : -1;

            if (q5 >= 0 && q6 > q5 && act == 7 && (stat_e == 1 || stat_e == 2)) {
                String plmn = entry.substring(q5 + 1, q6);
                if (is_allowed_plmn(plmn.c_str())) {
                    strncpy(cands[cand_count].plmn, plmn.c_str(), 7);
                    cands[cand_count].plmn[7] = '\0';
                    cands[cand_count].stat = stat_e;
                    cand_count++;
                }
            }
            pos = ne + 1;
        }
    }

    if (cand_count == 0) {
        syslog("MODEM", "PLMN-Scan: keine erlaubten Netze gefunden");
        // Fallback: Auto-Modus
        syslog("MODEM", "Fallback Auto-Modus 60s");
        s_modem.sendAT("+COPS=0");
        s_modem.waitResponse(10000L);
        RegStatus reg = wait_for_reg(60);
        if (reg == REG_OK_HOME || reg == REG_OK_ROAMING) goto registered;
        return false;
    }

    snprintf(syslog_msg, sizeof(syslog_msg), "PLMN-Scan: %d erlaubte Netze gefunden", cand_count);
    syslog("MODEM", syslog_msg);

    // Kandidaten durchprobieren (Reihenfolge: PLMN_TABLE-Priorität)
    for (int i = 0; i < cand_count && !g_shutdown; i++) {
        snprintf(syslog_msg, sizeof(syslog_msg), "Versuche %s...", cands[i].plmn);
        syslog("MODEM", syslog_msg);
        s_modem.sendAT("+COPS=1,2,\"", cands[i].plmn, "\",7");
        if (s_modem.waitResponse(30000L) == 1) {
            RegStatus reg = wait_for_reg(30);
            if (reg == REG_OK_HOME || reg == REG_OK_ROAMING) goto registered;
        }
    }

    // Alle Kandidaten gescheitert → Auto-Modus als letzter Versuch
    syslog("MODEM", "Alle Kandidaten gescheitert · Auto-Modus 60s");
    s_modem.sendAT("+COPS=0");
    s_modem.waitResponse(10000L);
    {
        RegStatus reg = wait_for_reg(60);
        if (reg == REG_OK_HOME || reg == REG_OK_ROAMING) goto registered;
    }
    return false;

registered:
    // Operator-Name und PLMN loggen
    lookup_operator(operator_out, op_size);
    return true;
}

// Letzten erfolgreichen Provider in NVS speichern/laden
static void save_last_plmn(const char* plmn) {
    Preferences p;
    p.begin("modem", false);
    p.putString("last_plmn", plmn);
    p.end();
}

static void load_last_plmn(char* plmn_out, size_t size) {
    Preferences p;
    p.begin("modem", true);
    String v = p.getString("last_plmn", "");
    p.end();
    strncpy(plmn_out, v.c_str(), size - 1);
    plmn_out[size - 1] = '\0';
}

// Netz-Registrierung: 3 Phasen mit steigender Dauer.
// Nach Watchdog-Reboot wird der gecachte Provider übersprungen → anderer bekommt Chance.
static bool try_register(char* operator_out, size_t op_size) {
    char syslog_msg[128];

    // --- Phase 0: Bevorzugter Provider (hartkodiert, z.B. Telekom DE) ---
    // o2 ist zuhause staerker, aber auf dem Land oft kaputt. Wir erzwingen Telekom
    // wenn verfuegbar. Kurzer Timeout (15s), damit ein fehlgeschlagener Versuch
    // nicht den Boot blockiert wenn Telekom nicht erreichbar ist.
    if (PREFERRED_PLMN[0] && s_wd_reboot_count == 0 && !g_shutdown) {
        snprintf(syslog_msg, sizeof(syslog_msg),
            "Praeferenz %s (Telekom DE, %ds Timeout)...", PREFERRED_PLMN, PREFERRED_PLMN_TIMEOUT_S);
        syslog("MODEM", syslog_msg);
        s_modem.sendAT("+COPS=1,2,\"", PREFERRED_PLMN, "\",7");
        if (s_modem.waitResponse(PREFERRED_PLMN_TIMEOUT_S * 1000L) == 1) {
            RegStatus r = wait_for_reg(PREFERRED_PLMN_TIMEOUT_S);
            if (r == REG_OK_HOME || r == REG_OK_ROAMING) {
                syslog("MODEM", "Praeferenz erfolgreich");
                save_last_plmn(PREFERRED_PLMN);
                lookup_operator(operator_out, op_size);
                return true;
            }
        }
        syslog("MODEM", "Praeferenz nicht erreichbar · Fallback");
        // COPS-State ist jetzt "kein Operator" — COPS=0/Scan uebernehmen
    }

    // --- Phase 1: Letzten Provider manuell (schnell, ~10s) ---
    // Nur wenn KEIN Watchdog-Reboot vorliegt (sonst war genau dieser Provider das Problem)
    // UND letzter Provider != Praeferenz (waere Doppel-Versuch)
    if (s_wd_reboot_count == 0) {
        char last_plmn[8] = "";
        load_last_plmn(last_plmn, sizeof(last_plmn));
        if (last_plmn[0] && is_allowed_plmn(last_plmn) && strcmp(last_plmn, PREFERRED_PLMN) != 0) {
            snprintf(syslog_msg, sizeof(syslog_msg), "Versuche letzten Provider %s...", last_plmn);
            syslog("MODEM", syslog_msg);
            s_modem.sendAT("+COPS=1,2,\"", last_plmn, "\",7");
            if (s_modem.waitResponse(30000L) == 1) {
                RegStatus r = wait_for_reg(30);
                if (r == REG_OK_HOME || r == REG_OK_ROAMING) {
                    lookup_operator(operator_out, op_size);
                    return true;
                }
            }
        }
    } else {
        snprintf(syslog_msg, sizeof(syslog_msg),
            "Watchdog-Reboot #%d · ueberspringe letzten Provider", s_wd_reboot_count);
        syslog("MODEM", syslog_msg);
    }

    // --- Phase 2: Auto-Modus (Modem wählt bestes verfügbares Netz, ~60s) ---
    syslog("MODEM", "Auto-Modus 60s");
    s_modem.sendAT("+COPS=0");
    s_modem.waitResponse(10000L);
    {
        RegStatus r = wait_for_reg(60);
        if (r == REG_OK_HOME || r == REG_OK_ROAMING) {
            char plmn[8] = "";
            get_current_plmn(plmn, sizeof(plmn));
            if (plmn[0] && is_allowed_plmn(plmn)) {
                save_last_plmn(plmn);
                lookup_operator(operator_out, op_size);
                return true;
            }
            // Provider nicht in Whitelist (z.B. Vodafone DE) → PLMN-Scan
            snprintf(syslog_msg, sizeof(syslog_msg),
                "Provider %s nicht erlaubt · starte PLMN-Scan", plmn);
            syslog("MODEM", syslog_msg);
        }
    }

    // --- Phase 3: PLMN-Scan (verfügbare Netze scannen, Whitelist filtern) ---
    if (plmn_scan_and_register(operator_out, op_size)) {
        char plmn[8] = "";
        get_current_plmn(plmn, sizeof(plmn));
        if (plmn[0]) save_last_plmn(plmn);
        return true;
    }
    return false;
}






static void modem_task(void* /*param*/) {

    #ifdef LTE_DISABLED
    ModemState state           = STATE_RUNNING;
    syslog("MODEM", "GPS-Only · LTE/SIM uebersprungen");
    #else
    ModemState state           = STATE_CHECK_SIM;
    syslog("MODEM", "Task gestartet · SIM pruefen");
    #endif


    uint32_t   last_stat_ms    = 0;
    uint32_t   stat_interval   = 5000;  // erste Abfrage nach 5s, danach GPS_INTERVAL_MS

    // Fahrtende-Erkennung: VBUS weg → 1min warten → Capture + Flush + Schlafen
    bool       vbus_was_in      = pmu_is_vbus_in();
    uint32_t   vbus_lost_ms     = 0;

    int        scan_fail_count = 0;        // Fehlversuche STATE_WAIT_FOR_NETWORK
    bool       radio_initialized = false;  // CFUN-Sequenz bereits ausgeführt

    bool       first_fix       = true;     // Erster GPS-Fix noch nicht gemeldet
    bool       had_fix         = false;    // Zuletzt gültiger Fix
    uint32_t   no_fix_log_ms   = 0;        // Wann zuletzt "kein Fix" geloggt
    uint32_t   gps_log_ms      = 0;        // Wann zuletzt Position geloggt (30s)

    char       syslog_msg[128];



    for (;;) {
        if (g_shutdown) break;

        // "at stop" → Task schläft bis "at start"
        while (s_task_paused && !g_shutdown) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (g_shutdown) break;

        uint32_t now = millis();



        switch (state) {

            // =================================================================

            case STATE_CHECK_SIM:

            // =================================================================

                s_sim_ok = (s_modem.getSimStatus() == SIM_READY);

                if (s_sim_ok) {

                    syslog("MODEM", "SIM OK · Netzsuche");

                    state = STATE_WAIT_FOR_NETWORK;

                } else {

                    syslog("MODEM", "Keine SIM · GPS-only Modus (ext. GPS)");
                    s_gps_enabled = false;
                    state = STATE_RUNNING;
                    last_stat_ms = now;
                    stat_interval = 5000;
                }

                continue; // Nächste Iteration, um Zustand neu zu prüfen



            // =================================================================

            case STATE_WAIT_FOR_NETWORK:

            // =================================================================

                // 1. SIM pruefen — stellt sicher dass Modem nach modem_init() stabil ist
                if (!ensure_sim_ready()) {
                    syslog("MODEM", "SIM nicht READY · warte 10s");
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    state = STATE_CHECK_SIM;
                    continue;
                }

                // 2. Radio konfigurieren (CNMP/CMNB/APN/PSM) — CFUN nur bei Abweichung
                if (!radio_initialized) {
                    if (!configure_radio()) {
                        syslog("MODEM", "Radio-Konfiguration fehlgeschlagen · warte 30s");
                        vTaskDelay(pdMS_TO_TICKS(30000));
                        continue;
                    }
                    radio_initialized = true;
                }

                // 3. Netz-Registrierung: erst manuell (DE), dann Auto (Ausland)
                {
                    bool registered = try_register(s_operator, sizeof(s_operator));

                    if (registered) {
                        s_sig_quality = (int8_t)s_modem.getSignalQuality();
                        scan_fail_count = 0;

                        int8_t bars = (s_sig_quality >= 20) ? 5 : (s_sig_quality >= 15) ? 4 : (s_sig_quality >= 10) ? 3 : (s_sig_quality >= 5) ? 2 : 1;
                        snprintf(syslog_msg, sizeof(syslog_msg), "Netz verbunden · %s · %d/5 Balken · CSQ %d", s_operator, bars, s_sig_quality);
                        syslog("MODEM", syslog_msg);

                        // GPRS + MQTT sofort aufbauen
                        if (modem_ensure_connected()) {
                            static bool s_ntp_done = false;
                            if (!s_ntp_done) s_ntp_done = modem_ntp_sync();

                            mqtt_configure();
                            mqtt_connect();
                        }

                        // Ext. GPS übernimmt — internes GPS nicht starten
                        s_gps_enabled = false;
                        { uint16_t p = telem_get_row_pending(); if (p > 0) { char m[64]; snprintf(m, sizeof(m), "STATE_RUNNING · %u Rows nachsenden", p); syslog("MODEM", m); } }
                        state = STATE_RUNNING;
                        last_stat_ms = now;
                        stat_interval = 5000;
                    } else {
                        scan_fail_count++;
                        // FIXES B.12: exponentieller Backoff statt fix 60 s
                        // (scan_fail_count wurde gezählt, aber nie genutzt) —
                        // 60 s → 5 min → 15 min (gedeckelt). Bricht den Dauerscan
                        // in Funkloch-Lage mit VBUS.
                        uint32_t backoff_ms = (scan_fail_count <= 1) ? 60000UL :
                                              (scan_fail_count == 2) ? 300000UL : 900000UL;
                        snprintf(syslog_msg, sizeof(syslog_msg),
                                 "Kein Operator erreichbar (Versuch %d) · warte %lus",
                                 scan_fail_count, (unsigned long)(backoff_ms / 1000UL));
                        syslog("MODEM", syslog_msg);
                        // shutdown-responsiv warten (1-s-Schritte)
                        for (uint32_t w = 0; w < backoff_ms && !g_shutdown; w += 1000)
                            vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                }

                continue;



            // =================================================================

            case STATE_RUNNING:

            // =================================================================

                // Sleep-Entscheidung trifft mod_sleep
                // Modem läuft einfach weiter bis Deep Sleep



                // --- Periodische Aktionen ---

                if ((now - last_stat_ms) >= stat_interval) {

                    // GPS-Daten abfragen — nur wenn internes GPS aktiv
                    //
                    // ⚠️ FIXES A.5 (latente P2-Falle): Dieser Pfad ist im Produktivbetrieb
                    // INAKTIV — aktiv ist das externe M10-GPS (mod_gps_ext.cpp), das volle
                    // Gates hat (0,0-Filter, HDOP/Sat-Gate, Settle-Zeit, Mindestdistanz für
                    // Kurs). Hier fehlt das alles: getGPS() geht ohne 0,0-Filter, ohne
                    // HDOP-/Sat-Gate und ohne Settle-Zeit in gps_update(), und die
                    // Kursberechnung unten hat keine Mindestdistanz → Zufallskurs im Stand.
                    // BEVOR jemand auf internes GPS umschaltet (oder das M10 ausfällt):
                    // gleiche Gates wie mod_gps_ext.cpp:170-179 nachrüsten ([FLASH]).
                    float lat = 0, lon = 0, speed = 0, alt = 0, accuracy = 0;
                    int   vsat = 0, usat = 0;
                    int   year = 0, month = 0, day = 0, hour = 0, gmin = 0, sec = 0;

                    if (s_gps_enabled && s_modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                                       &year, &month, &day, &hour, &gmin, &sec)) {

                        // Kurs aus letztem → aktuellem Fix berechnen
                        float course = 0.0f;
                        if (s_prev_fix_ok) {
                            double dlon = ((double)lon - s_prev_fix_lon) * (M_PI / 180.0);
                            double lat1 = s_prev_fix_lat * (M_PI / 180.0);
                            double lat2 = (double)lat    * (M_PI / 180.0);
                            double x = sin(dlon) * cos(lat2);
                            double y = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);
                            course = (float)(atan2(x, y) * (180.0 / M_PI));
                            if (course < 0) course += 360.0f;
                        }
                        s_prev_fix_lat = (double)lat;
                        s_prev_fix_lon = (double)lon;
                        s_prev_fix_ok  = true;
                        if (speed < 0.0f || speed >= 200.0f) speed = 0.0f;  // Glitch-Filter
                        gps_update((double)lat, (double)lon, speed, course);  // getGPS() liefert km/h (AT+CGNSINF)

                        // Unplausible Werte bereinigen (TinyGSM gibt -9999 oder 0 bei leeren Feldern)
                        if (vsat < 0) vsat = 0;
                        if (usat < 0) usat = 0;
                        // SIM7080G liefert usat oft leer (=0) trotz Fix → vsat als Ersatz
                        if (usat == 0 && vsat > 0) usat = vsat;

                        // GPS-Details für Serial-Abfrage speichern
                        s_gps_alt     = alt;
                        s_gps_speed   = speed;
                        s_gps_hdop    = accuracy;
                        s_gps_vsat    = vsat;
                        s_gps_usat    = usat;
                        s_gps_has_fix = true;


                        if (first_fix) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Erster Fix · %.6f %.6f · %d sichtbar / %d im Fix · HDOP: %.1f · Alt: %.0fm",
                                     lat, lon, vsat, usat, accuracy, alt);
                            syslog("GPS", syslog_msg);
                            first_fix = false;
                            had_fix   = true;
                        } else if (!had_fix) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Fix wieder · %.6f %.6f · %d im Fix",
                                     lat, lon, usat);
                            syslog("GPS", syslog_msg);
                            had_fix = true;
                        }
                        // Im Normalbetrieb kein Log-Spam — g_gps wird still aktualisiert

                    } else if (s_gps_enabled) {

                        gps_invalidate();
                        s_gps_has_fix = false;

                        if (had_fix) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Fix verloren · Sat sichtbar: %d", vsat);
                            syslog("GPS", syslog_msg);
                            had_fix      = false;
                            no_fix_log_ms = now;
                        } else if (now - no_fix_log_ms >= 30000UL) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Suche Fix · Sat sichtbar: %d · verwendet: %d", vsat, usat);
                            syslog("GPS", syslog_msg);
                            no_fix_log_ms = now;
                        }
                    }



                    // Periodischer GPS-Positions-Log (alle 30s bei aktivem Fix, ext. GPS)
                    {
                        GpsSnapshot snap = gps_snapshot();
                        if (snap.valid && now - gps_log_ms >= 30000UL) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Pos: %.6f %.6f · %d sichtbar / %d im Fix",
                                     snap.lat, snap.lon, gps_ext_sat_visible(), gps_ext_sat_count());
                            syslog("GPS", syslog_msg);
                            gps_log_ms = now;
                        }
                    }

                    last_stat_ms = now;
                    stat_interval = GPS_INTERVAL_MS;

                }



                // ── Sleep-Anforderung vom Inaktivitäts-Timer ────────────────────────
                if (g_sleep_requested) {
                    // g_sleep_requested NICHT zurücksetzen — verhindert Log-Spam in sleep_update()
                    syslog("MODEM", "Sleep · letzter GPS-Punkt + Flush");
                    modem_pre_sleep_flush();
                    sleep_force();
                }

                // ── Fahrtende-Erkennung: VBUS weg → Flush + Sleep ──
                // Sleep-Entscheidung trifft mod_sleep (VBUS-basiert, 5 min Schonfrist).
                // Hier nur: letzten Punkt capturen + Daten senden solange Netz da.
                {
                    bool vbus_now = pmu_is_vbus_in();

                    if (vbus_now) {
                        vbus_was_in  = true;
                        vbus_lost_ms = 0;
                        g_trip_ending = false;
                    } else if (vbus_was_in) {
                        // Erst nach 5s ohne VBUS reagieren — Glitch/Stromspitze ignorieren
                        if (vbus_lost_ms == 0) {
                            vbus_lost_ms = now;
                        } else if (!g_trip_ending && (now - vbus_lost_ms) >= 5000UL) {
                            g_trip_ending = true;
                            syslog("MODEM", "VBUS weg seit 5s — Flush");
                            telem_force_capture("VBUS weg");
                            vTaskDelay(pdMS_TO_TICKS(300));
                            // Ausstehende Zeilen werden unten im MQTT-Block gepublisht
                        }
                        // VBUS weg → VW-Kurzzeitabschaltung möglich, weiter warten
                    }
                }

                #ifndef LTE_DISABLED
                // ── MQTT: Publish + 4-Stufen-Eskalation bei Verbindungsverlust ──
                //
                //  Stufe 1: MQTT Reconnect alle 10s
                //  Stufe 2: Nach 3 MQTT-Fails → Modem-Reset (PWRKEY)
                //  Stufe 3: Nach 2 Modem-Resets → PLMN-Scan + neuer Provider
                //  Stufe 4: Nach 2 PLMN-Scans → Reboot (max 3x in 30 min)
                {
                    static uint32_t s_mqtt_reconnect_ms = 0;
                    static uint8_t  s_mqtt_fail_count   = 0;  // Stufe 1 → 2
                    static uint8_t  s_modem_reset_count = 0;  // Stufe 2 → 3
                    static uint8_t  s_plmn_scan_count   = 0;  // Stufe 3 → 4

                    // MQTT-Verbindung prüfen / wiederherstellen
                    if (s_sim_ok && !mqtt_is_connected()) {
                        if (now - s_mqtt_reconnect_ms >= MQTT_RECONNECT_MS) {

                            // ── Stufe 4: Reboot (mit RTC-Limiter) ──
                            if (s_plmn_scan_count >= PLMN_SCAN_ESCALATE) {
                                uint32_t epoch = rtc_unix_timestamp();
                                // Reboot-Fenster abgelaufen? Counter zurücksetzen
                                if (epoch > 0 && s_wd_first_reboot_epoch > 0
                                    && (epoch - s_wd_first_reboot_epoch) >= WD_REBOOT_WINDOW_S) {
                                    s_wd_reboot_count = 0;
                                    s_wd_first_reboot_epoch = 0;
                                }
                                if (s_wd_reboot_count >= WD_REBOOT_MAX) {
                                    // Reboot-Limit erreicht — weiter puffern, Eskalation zurücksetzen
                                    char m[96]; snprintf(m, sizeof(m),
                                        "REBOOT-LIMIT: %d Reboots in %lu min · kein weiterer Reboot, weiter puffern",
                                        s_wd_reboot_count, (epoch - s_wd_first_reboot_epoch) / 60UL);
                                    syslog("MQTT", m);
                                    s_plmn_scan_count = 0;
                                    s_modem_reset_count = 0;
                                    s_mqtt_fail_count = 0;
                                } else {
                                    uint16_t pending = telem_get_row_pending();
                                    char m[96]; snprintf(m, sizeof(m),
                                        "ESKALATION: Reboot nach Erschoepfung aller Mittel · %u Rows pending",
                                        pending);
                                    syslog("MQTT", m);
                                    if (s_wd_reboot_count == 0 && epoch > 0) s_wd_first_reboot_epoch = epoch;
                                    s_wd_reboot_count++;
                                    Serial.flush();
                                    // Graceful: spiffs_lock holen + Shutdown-Signal
                                    // verhindert dass esp_restart() mitten in
                                    // einem spiffs_q_append-Rename zuschlaegt
                                    // → korrupte Queue-Datei beim naechsten Boot.
                                    g_shutdown = true;
                                    if (spiffs_lock(2000)) spiffs_unlock();
                                    delay(200);
                                    esp_restart();
                                }
                                s_mqtt_reconnect_ms = millis();

                            // ── Stufe 3: PLMN-Scan + Provider-Wechsel ──
                            } else if (s_modem_reset_count >= MODEM_RESET_ESCALATE) {
                                syslog("MODEM", "PLMN-Scan: Provider-Wechsel nach Modem-Reset-Fehlschlaegen");
                                char op[48] = "";
                                if (plmn_scan_and_register(op, sizeof(op))) {
                                    snprintf(syslog_msg, sizeof(syslog_msg), "Neuer Provider: %s", op);
                                    syslog("MODEM", syslog_msg);
                                    strncpy(s_operator, op, sizeof(s_operator) - 1);
                                    // Neuen Provider merken
                                    char plmn[8] = "";
                                    get_current_plmn(plmn, sizeof(plmn));
                                    if (plmn[0]) save_last_plmn(plmn);
                                    // GPRS + MQTT neu verbinden
                                    if (modem_ensure_connected() && mqtt_connect()) {
                                        syslog("MQTT", "Reconnect OK nach Provider-Wechsel");
                                        s_mqtt_fail_count = 0;
                                        s_modem_reset_count = 0;
                                        s_plmn_scan_count = 0;
                                    } else {
                                        s_plmn_scan_count++;
                                        s_mqtt_fail_count = 0;
                                        s_modem_reset_count = 0;
                                    }
                                } else {
                                    syslog("MODEM", "PLMN-Scan gescheitert");
                                    s_plmn_scan_count++;
                                    s_mqtt_fail_count = 0;
                                    s_modem_reset_count = 0;
                                }
                                s_mqtt_reconnect_ms = millis();

                            // ── Stufe 2: Modem-Reset (PWRKEY) ──
                            } else if (s_mqtt_fail_count >= MQTT_FAIL_RESET_COUNT) {
                                syslog("MODEM", "Modem-Reset nach Reconnect-Fehlern (PWRKEY)");
                                s_modem.sendAT("+CNACT=0,0");
                                s_modem.waitResponse(5000L);
                                modem_pwrkey_pulse();
                                delay(3000);
                                for (int i = 0; i < 5; i++) {
                                    if (s_modem.testAT(1000)) break;
                                    delay(500);
                                }
                                s_connected = false;
                                s_mqtt_fail_count = 0;
                                s_modem_reset_count++;
                                s_mqtt_reconnect_ms = millis();
                                char m[80]; snprintf(m, sizeof(m),
                                    "Modem-Reset #%d abgeschlossen, naechster Versuch in 10s",
                                    s_modem_reset_count);
                                syslog("MODEM", m);

                            // ── Stufe 1: MQTT Reconnect ──
                            } else {
                                uint16_t pending = telem_get_row_pending();
                                { char m[80]; snprintf(m, sizeof(m),
                                    "Reconnect... (%u Rows pending, Versuch %d)",
                                    pending, s_mqtt_fail_count + 1);
                                  syslog("MQTT", m); }

                                if (modem_ensure_connected()) {
                                    if (mqtt_connect()) {
                                        s_mqtt_fail_count = 0;
                                        s_modem_reset_count = 0;
                                        s_plmn_scan_count = 0;
                                        syslog("MQTT", "Reconnect OK");
                                    } else {
                                        s_mqtt_fail_count++;
                                        syslog("MQTT", "Reconnect: GPRS OK aber MQTT-Connect fehlgeschlagen");
                                    }
                                } else {
                                    // GPRS fehlgeschlagen (z.B. 0.0.0.0 oder echte Trennung).
                                    // Keine voreilige state-transition — die Eskalation (Stufe 2
                                    // Modem-Reset, Stufe 3 PLMN-Scan) uebernimmt das Recovery.
                                    s_mqtt_fail_count++;
                                    RegStatus reg = s_modem.getRegistrationStatus();
                                    char m[80]; snprintf(m, sizeof(m),
                                        "GPRS fehlgeschlagen · reg=%d · Eskalation greift (fail=%d)",
                                        (int)reg, s_mqtt_fail_count);
                                    syslog("MODEM", m);
                                }
                                s_mqtt_reconnect_ms = millis();
                            }
                        }
                    }

                    // Ausstehende Zeilen sofort publishen — aber NICHT in den ersten 15s
                    // nach Boot: bei Backlog (z.B. nach Eskalations-Reboot mit 100+ Rows)
                    // wuerde der Burst parallel zur laufenden Modul-Init laufen und
                    // ipc0/Spinlocks unter Last setzen → bisheriger PANIC-Trigger.
                    if (mqtt_is_connected() && telem_get_row_pending() > 0 && millis() > 15000UL) {
                        TelemetryRow row;
                        int sent = 0;
                        while (telem_get_row_pending() > 0 && sent < 10) {
                            if (!telem_peek_row(row)) break;
                            if (!mqtt_publish_row(row)) break;
                            telem_ack_row();
                            sent++;
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        if (sent > 0) {
                            s_mqtt_fail_count = 0;
                            s_modem_reset_count = 0;
                            s_plmn_scan_count = 0;
                            // Reboot-Limiter zurücksetzen nach erfolgreichem Publish
                            s_wd_reboot_count = 0;
                            s_wd_first_reboot_epoch = 0;
                        }
                    }

                    // URCs verarbeiten (Server-ACK)
                    mqtt_poll();

                    // Signal periodisch abfragen
                    static uint32_t s_sig_ms = 0;
                    if (s_sim_ok && (now - s_sig_ms) >= 30000UL) {
                        s_sig_ms = now;
                        s_sig_quality = (int8_t)s_modem.getSignalQuality();
                    }

                    // CPSI: alle 60s detaillierte Zell-/Signalinfo ins syslog.
                    // Reine Read-Operation, beeinflusst Registrierung nicht.
                    static uint32_t s_cpsi_ms = 0;
                    if (s_sim_ok && (now - s_cpsi_ms) >= 60000UL) {
                        s_cpsi_ms = now;
                        cpsi_query_log(false);
                    }
                }
                #endif // LTE_DISABLED

                break;

        }



        vTaskDelay(pdMS_TO_TICKS(100)); // Kurze Pause in jedem Zustand

    }
    Serial.println("[MODEM] Task beendet (Shutdown)");
    vTaskDelete(NULL);
}



// ---- Öffentliche API ---------------------------------------



void modem_init() {

    char syslog_msg[128];

    snprintf(syslog_msg, sizeof(syslog_msg), "Init TX=GPIO%d RX=GPIO%d %dbaud",
             MODEM_TX_PIN, MODEM_RX_PIN, MODEM_BAUD);
    syslog("MODEM", syslog_msg);

    // 1. Pins vorbereiten
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
#if MODEM_FLIGHT_PIN != -1
    pinMode(MODEM_FLIGHT_PIN, OUTPUT);
    digitalWrite(MODEM_FLIGHT_PIN, LOW);   // Flight-Mode aus
#endif
    pinMode(MODEM_STATUS_PIN, INPUT);

    // 2. UART starten
    s_serial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(100);

    // 3. AT-Handshake — nach Deep Sleep (Modem war hart per DC3 stromlos, KEIN
    //    AT+CPOF — siehe FIXES C.3) braucht das Modem einen PWRKEY-Puls.
    //    Optimiert: 500ms AT-Timeout (UART lokal), 3 Versuche vor PWRKEY,
    //    1.8s Post-Puls-Delay (SIM7080 Serial aktiv nach 1.8s lt. Datasheet).
    //    Rückgängig: testAT(1000), retry>6, delay(3000)
    syslog("MODEM", "AT-Handshake...");
    {
        int retry = 0;
        int pwrkey_cycles = 0;
        while (!s_modem.testAT(500)) {
            if (++retry > 3) {
                // FIXES B.2: KEIN endloses Hängen. Wenn die PMU fehlt (DC3 aus →
                // Modem stromlos) oder das Modem defekt ist, gäbe es hier sonst eine
                // Endlosschleife → loop()/sleep_update() laufen nie → kein Deep Sleep
                // → Akku bis Tiefentladung. Nach ~5 PWRKEY-Zyklen (≈30 s) aufgeben,
                // s_sim_ok=false setzen und GPS-only weiterbooten (modem_task retryt).
                if (++pwrkey_cycles > 5) {
                    s_sim_ok = false;
                    syslog("MODEM", "FEHLER: kein AT nach 5 PWRKEY-Zyklen — Modem defekt/abwesend, boote GPS-only");
                    return;
                }
                syslog("MODEM", "Kein AT · PWRKEY-Puls...");
                modem_pwrkey_pulse();
                delay(1800);   // Serial aktiv nach ~1.8s (SIM7080 Datasheet)
                retry = 0;
            }
        }
    }
    syslog("MODEM", "AT OK");

    // Nach Handshake: ATI pollen bis Modem-Info verfuegbar (500ms Intervall, max 5s)
    // Rückgängig: delay(2000), 5 Versuche
    {
        String info;
        for (int i = 0; i < 10; i++) {
            info = s_modem.getModemInfo();
            if (info.length() > 0) break;
            delay(500);
        }
        syslog("MODEM", ("Modem: ATI  " + info).c_str());
    }

    // 4. SIM prüfen und ggf. entsperren
    //    Direkt AT+CPIN? senden — TinyGSM getSimStatus() erkennt manche Antworten nicht.
    //    Moegliche Antworten: +CPIN: READY / +CPIN: SIM PIN / +CME ERROR: ...
    //    SIM-Subsystem braucht nach Modem-Boot einige Sekunden.
    //    Bei Warmstart (ESP-Reset ohne Modem-Reset) bleibt SIM oft im ERROR-State
    //    → nach 3 Fehlversuchen PWRKEY-Puls erzwingen.
    // SIM-Subsystem: statt fester 5s → CPIN? pollen (500ms Intervall, max 5s)
    // Rückgängig: delay(5000) statt Polling-Schleife
    syslog("MODEM", "Warte auf SIM-Subsystem...");
    // UART-Buffer leeren (Reste von vorherigen Kommandos)
    while (s_serial.available()) s_serial.read();
    for (int w = 0; w < 10; w++) {
        String probe = "";
        s_modem.sendAT("+CPIN?");
        int rp = s_modem.waitResponse(500L, probe);
        if (rp > 0) break;  // Antwort da (egal ob READY/PIN/ERROR) → weiter
        delay(500);
    }

    SimStatus sim = SIM_ERROR;
    for (int i = 0; i < 12; i++) {
        String cpin_resp = "";
        s_modem.sendAT("+CPIN?");
        int r = s_modem.waitResponse(5000L, cpin_resp);
        cpin_resp.trim();
        // Newlines aus AT-Response entfernen für einzeiliges Log
        cpin_resp.replace("\r", "");
        cpin_resp.replace("\n", " ");
        cpin_resp.trim();
        snprintf(syslog_msg, sizeof(syslog_msg), "CPIN? → rc=%d · %s", r, cpin_resp.c_str());
        syslog("MODEM", syslog_msg);

        if (cpin_resp.indexOf("READY") >= 0) {
            sim = SIM_READY;
            break;
        } else if (cpin_resp.indexOf("SIM PIN") >= 0) {
            sim = SIM_LOCKED;
            break;
        } else if (cpin_resp.indexOf("SIM PUK") >= 0) {
            sim = SIM_LOCKED;
            syslog("MODEM", "WARNUNG: SIM PUK erforderlich!");
            break;
        } else if (cpin_resp.indexOf("NOT INSERTED") >= 0) {
            syslog("MODEM", "Keine SIM eingelegt");
            sim = SIM_ERROR;
            break;
        }

        // Kein PWRKEY — sperrt die SIM. Einfach laenger warten (max 60s).
        snprintf(syslog_msg, sizeof(syslog_msg), "SIM nicht erkannt (%d/12), warte 5s...", i + 1);
        syslog("MODEM", syslog_msg);
        delay(5000);
    }

    snprintf(syslog_msg, sizeof(syslog_msg), "SIM Status: %d (0=err 1=ready 2=locked)", sim);
    syslog("MODEM", syslog_msg);

    if (sim == SIM_LOCKED) {
        const char* pin = cfg_sim_pin();
        size_t pinlen = strlen(pin);
        snprintf(syslog_msg, sizeof(syslog_msg),
                 "SIM gesperrt · PIN konfiguriert: %s (%d Zeichen)",
                 pinlen > 0 ? "ja" : "NEIN", (int)pinlen);
        syslog("MODEM", syslog_msg);

        if (pinlen > 0) {
            // Verbleibende PIN-Versuche pruefen (PUK-Schutz)
            // AT+CPINR nicht von allen SIM7080G-FW unterstuetzt — auch AT+SPIC versuchen
            int retries = -1;
            {
                String resp = "";
                s_modem.sendAT("+CPINR=\"SC\"");
                int r = s_modem.waitResponse(5000L, resp);
                resp.trim();
                if (r == 1 && resp.indexOf(',') >= 0) {
                    int comma = resp.lastIndexOf(',');
                    retries = resp.substring(comma + 1).toInt();
                }
                // Fallback: AT+SPIC (SIMCom-spezifisch)
                if (retries < 0) {
                    resp = "";
                    s_modem.sendAT("+SPIC");
                    r = s_modem.waitResponse(3000L, resp);
                    resp.trim();
                    // Antwort: +SPIC: 3   oder   +SPIC: pin1_times,puk1_times,...
                    if (r == 1) {
                        int colon = resp.indexOf(':');
                        if (colon >= 0) {
                            String val = resp.substring(colon + 1);
                            val.trim();
                            int comma = val.indexOf(',');
                            retries = (comma >= 0) ? val.substring(0, comma).toInt() : val.toInt();
                        }
                    }
                }
            }
            snprintf(syslog_msg, sizeof(syslog_msg),
                     "PIN-Versuche uebrig: %d%s", retries, retries < 0 ? " (unbekannt)" : "");
            syslog("MODEM", syslog_msg);

            if (retries >= 0 && retries < 2) {
                syslog("MODEM", "ABBRUCH: Weniger als 2 PIN-Versuche! PUK-Gefahr!");
                s_sim_ok = false;
            } else {
                // TinyGSM simUnlock sendet AT+CPIN="pin" — manuell ohne Anfuehrungszeichen versuchen
                // SIM7080G akzeptiert beides, aber manche FW-Versionen sind empfindlich
                snprintf(syslog_msg, sizeof(syslog_msg), "+CPIN=%s", pin);
                syslog("MODEM", "Entsperre SIM...");
                s_modem.sendAT(syslog_msg);
                int r = s_modem.waitResponse(10000L);
                snprintf(syslog_msg, sizeof(syslog_msg), "CPIN Antwort: %d (1=OK)", r);
                syslog("MODEM", syslog_msg);

                if (r == 1) {
                    syslog("MODEM", "PIN akzeptiert");
                    delay(5000);  // SIM braucht nach PIN-Eingabe Zeit
                    // Verifizieren dass SIM jetzt READY ist
                    SimStatus after = s_modem.getSimStatus();
                    if (after == SIM_READY) {
                        syslog("MODEM", "SIM READY nach PIN-Eingabe");
                        s_sim_ok = true;
                    } else {
                        snprintf(syslog_msg, sizeof(syslog_msg),
                                 "SIM nach PIN nicht READY (Status: %d)", after);
                        syslog("MODEM", syslog_msg);
                        s_sim_ok = false;
                    }
                } else {
                    syslog("MODEM", "FEHLER: PIN abgelehnt!");
                    s_sim_ok = false;
                }
            }
        } else {
            syslog("MODEM", "FEHLER: SIM braucht PIN, aber kein PIN konfiguriert!");
            syslog("MODEM", "PIN ueber Web-UI (Config > SIM PIN) oder secrets.h setzen");
            s_sim_ok = false;
        }
    } else if (sim == SIM_READY) {
        syslog("MODEM", "SIM bereit (kein PIN noetig)");
        s_sim_ok = true;
    } else {
        syslog("MODEM", "Keine SIM erkannt oder SIM-Fehler");
        s_sim_ok = false;
    }

    #ifdef LTE_DISABLED
    // GPS-Only Modus: direkt GPS aktivieren ohne SIM/Netz
    syslog("MODEM", "LTE_DISABLED: aktiviere GPS direkt...");
    if (gps_enable_with_config()) {
        syslog("GPS", "Aktiviert · suche Fix");
    } else {
        syslog("GPS", "FEHLER · Aktivierung fehlgeschlagen");
    }
    #endif

    // Alte Green-List aufräumen (wurde durch PLMN-Scan ersetzt)
    SPIFFS.remove("/plmn_good.txt");

    syslog("MODEM", "Init abgeschlossen");
}



void modem_start_task() {
    char syslog_msg[64];
    if (GPS_EXT_ENABLED)
        syslog("MODEM", "Task gestartet, internes GPS deaktiviert (ext. GPS aktiv)");
    else {
        snprintf(syslog_msg, sizeof(syslog_msg), "Task gestartet, GPS alle %ds", GPS_INTERVAL_MS / 1000);
        syslog("MODEM", syslog_msg);
    }

    xTaskCreatePinnedToCore(
        modem_task,
        "MODEM",
        12288,       // TLS-Handshake (TinyGsmClientSecure + HttpClient) braucht ~8-10 KB
        nullptr,
        1,
        &s_modem_task_handle,
        1       // Core 1
    );
}

// NTP-Sync via SIM7080G — setzt RTC auf UTC.
// Nur einmalig nach erster GPRS-Verbindung aufrufen.
static bool modem_ntp_sync() {
    // AT+CNTP="pool.ntp.org",0  (timezone=0 → UTC)
    s_modem.sendAT("+CNTP=\"pool.ntp.org\",0");
    if (s_modem.waitResponse(3000L) != 1) {
        syslog("MODEM", "NTP-Sync: CNTP Konfiguration fehlgeschlagen");
        return false;
    }
    // NTP-Sync starten
    s_modem.sendAT("+CNTP");
    // URC "+CNTP: 1" = Erfolg, Timeout 10s
    if (s_modem.waitResponse(10000L, "+CNTP: 1") != 1) {
        syslog("MODEM", "NTP-Sync: kein Erfolg-URC");
        return false;
    }
    // UTC-Zeit aus CCLK lesen (timezone=0 → direkt UTC)
    s_modem.sendAT("+CCLK?");
    String resp;
    if (s_modem.waitResponse(3000L, resp) != 1) {
        syslog("MODEM", "NTP-Sync: CCLK Lesefehler");
        return false;
    }
    // Format: +CCLK: "YY/MM/DD,HH:MM:SS+TZ"
    int yy,mo,dd,hh,mi,ss;
    const char* p = strstr(resp.c_str(), "+CCLK: \"");
    if (!p) { syslog("MODEM", "NTP-Sync: CCLK Parse-Fehler"); return false; }
    if (sscanf(p + 8, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss) != 6) {
        syslog("MODEM", "NTP-Sync: CCLK Format-Fehler");
        return false;
    }
    rtc_set_datetime(2000 + yy, mo, dd, hh, mi, ss);
    char msg[64];
    snprintf(msg, sizeof(msg), "NTP-Sync OK · UTC %04d-%02d-%02d %02d:%02d:%02d",
             2000+yy, mo, dd, hh, mi, ss);
    syslog("MODEM", msg);
    return true;
}


bool modem_ensure_connected() {
    if (s_modem.isGprsConnected()) {
        return true;
    }

    // Alten PDP-Kontext sauber trennen (verhindert stuck half-open state)
    s_modem.sendAT("+CNACT=0,0");
    s_modem.waitResponse(5000L);

    // gprsConnect: setzt APN, sendet +CNACT=0,1, wartet auf "+APP PDP: 0,ACTIVE" URC
    syslog("MODEM", "GPRS verbinden...");
    if (!s_modem.gprsConnect(cfg_apn(), cfg_apn_user(), cfg_apn_pass())) {
        syslog("MODEM", "GPRS fehlgeschlagen");
        s_connected = false;
        return false;
    }

    // IP-Plausibilität prüfen: SIM7080G meldet manchmal "GPRS OK" mit 0.0.0.0
    // (halb-offener PDP-Kontext). MQTT-Connect scheitert dann garantiert.
    String ip = s_modem.getLocalIP();
    if (ip.length() == 0 || ip == "0.0.0.0" || ip.startsWith("0.0.0")) {
        char msg[64];
        snprintf(msg, sizeof(msg), "GPRS OK mit ungueltiger IP (%s) · Abbruch", ip.c_str());
        syslog("MODEM", msg);
        s_modem.sendAT("+CNACT=0,0");  // PDP explizit trennen für nächsten Versuch
        s_modem.waitResponse(5000L);
        s_connected = false;
        return false;
    }

    s_connected = true;
    char syslog_msg[64];
    snprintf(syslog_msg, sizeof(syslog_msg), "GPRS OK: %s", ip.c_str());
    syslog("MODEM", syslog_msg);
    s_had_lte = true;
    return true;
}

bool        modem_is_connected()    { return s_connected; }
int8_t      modem_signal_quality()  { return s_sig_quality; }
const char* modem_operator()        { return s_operator; }
uint16_t    modem_plmn()             { return s_plmn_num; }
bool        modem_sim_ok()          { return s_sim_ok; }

void modem_pre_sleep_flush() {
    telem_force_capture("Schlafen");
    vTaskDelay(pdMS_TO_TICKS(300));

    if (mqtt_is_connected()) {
        TelemetryRow row;
        int sent = 0;
        while (telem_get_row_pending() > 0 && sent < 20) {
            if (!telem_peek_row(row)) break;
            if (!mqtt_publish_row(row)) break;
            telem_ack_row();  // erst nach erfolgreichem Publish entfernen
            sent++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        mqtt_disconnect();
    }
}

bool modem_mqtt_connected() {
    return mqtt_is_connected();
}

void modem_poweroff() {
    syslog("MODEM", "Power-Off fuer Deep Sleep...");

    // Task beenden falls noch aktiv
    TaskHandle_t h = xTaskGetHandle("MODEM");
    if (h) {
        vTaskDelete(h);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_modem_task_handle = nullptr;

    // Hardware Power-Off via PMU: DC3 (Modem VDD) abschalten
    // Kein s_serial.end() — würde auf TX-Flush warten und hängen
    modem_pmu_poweroff();

    syslog("MODEM", "Power-Off OK");
}

void modem_print_lte_info() {
    Serial.println("┌─── LTE Info ─────────────────────────┐");
    Serial.printf( "│ SIM:        %s\n", s_sim_ok ? "OK" : "nicht erkannt");
    Serial.printf( "│ Signal:     %d/31 CSQ\n", (int)s_sig_quality);
    Serial.printf( "│ Anbieter:   %s\n", s_operator[0] ? s_operator : "---");
    Serial.printf( "│ Daten:      %s\n", s_connected ? "verbunden" : "getrennt");

    // Band abfragen
    s_modem.sendAT("+CBAND?");
    String band = "";
    if (s_modem.waitResponse(2000L, band) == 1 && band.length() > 0) {
        band.trim();
        Serial.printf("│ Band:       %s\n", band.c_str());
    }

    // CEREG Status
    s_modem.sendAT("+CEREG?");
    String cereg = "";
    if (s_modem.waitResponse(2000L, cereg) == 1) {
        cereg.trim();
        int stat = -1;
        int comma = cereg.indexOf(',');
        if (comma >= 0) stat = cereg.substring(comma + 1).toInt();
        const char* reg_str = "?";
        switch (stat) {
            case 0: reg_str = "nicht registriert"; break;
            case 1: reg_str = "Heimnetz"; break;
            case 2: reg_str = "sucht..."; break;
            case 3: reg_str = "abgelehnt"; break;
            case 5: reg_str = "Roaming"; break;
        }
        Serial.printf("│ Registr.:   %s\n", reg_str);
    }

    Serial.println("│ → 'lte sig'  für RSRP/RSRQ/Band");
    Serial.println("│ → 'lte scan' für Netzwerk-Scan (destruktiv!)");
    Serial.println("└──────────────────────────────────────┘");
}

void modem_print_lte_sig() {
    cpsi_query_log(true);
}

void modem_print_lte_scan() {
    Serial.println("Netzwerk-Scan läuft (bis 3 Min)...");
    Serial.flush();

    s_modem.sendAT("+COPS=?");
    String cops = "";
    int r = s_modem.waitResponse(180000L, cops);
    if (r == 1 && cops.length() > 5) {
        int pos = 0, found = 0;
        while (pos < (int)cops.length()) {
            int ns = cops.indexOf('(', pos);
            if (ns < 0) break;
            int ne = cops.indexOf(')', ns);
            if (ne < 0) break;
            String entry = cops.substring(ns + 1, ne);
            int stat_e = entry.substring(0, entry.indexOf(',')).toInt();
            int q1s = entry.indexOf('"');
            int q1e = (q1s >= 0) ? entry.indexOf('"', q1s + 1) : -1;
            int lc  = entry.lastIndexOf(',');
            if (q1s >= 0 && q1e > q1s && lc > q1e) {
                String name = entry.substring(q1s + 1, q1e);
                int act = entry.substring(lc + 1).toInt();
                const char* tech = (act == 7) ? "LTE-M" : (act == 9) ? "NB-IoT" : "?";
                const char* mark = (stat_e == 2) ? " ◀" : "";
                Serial.printf("  %-20s [%s]%s\n", name.c_str(), tech, mark);
                found++;
            }
            pos = ne + 1;
        }
        if (found == 0) Serial.println("  Keine Netze gefunden");
    } else {
        Serial.println("  Scan Timeout / kein Ergebnis");
    }

    // COPS=? setzt Modem in "kein Operator" — Modem-Task uebernimmt die manuelle Registrierung
}

void modem_print_lte_bands() {
    Serial.println("┌─── LTE Band-Konfiguration ───────────┐");

    // Aktuelle Band-Config abfragen
    s_modem.sendAT("+CBANDCFG?");
    String resp = "";
    if (s_modem.waitResponse(3000L, resp) == 1) {
        resp.trim();
        Serial.printf("│ %s\n", resp.c_str());
    } else {
        Serial.println("│ CBANDCFG nicht lesbar");
    }

    // Netz-Modus
    resp = "";
    s_modem.sendAT("+CMNB?");
    if (s_modem.waitResponse(2000L, resp) == 1) {
        resp.trim();
        int mode = -1;
        int idx = resp.indexOf(':');
        if (idx >= 0) mode = resp.substring(idx + 1).toInt();
        const char* ms = (mode == 1) ? "CAT-M only" : (mode == 2) ? "NB-IoT only" : (mode == 3) ? "CAT-M + NB-IoT" : "?";
        Serial.printf("│ Modus:      %s (CMNB=%d)\n", ms, mode);
    }

    // Bevorzugter RAT
    resp = "";
    s_modem.sendAT("+CNMP?");
    if (s_modem.waitResponse(2000L, resp) == 1) {
        resp.trim();
        Serial.printf("│ RAT:        %s\n", resp.c_str());
    }

    Serial.println("│");
    Serial.println("│ DE-Carrier LTE-M/NB-IoT Baender:");
    Serial.println("│   Telekom:  B8, B20");
    Serial.println("│   Vodafone: B8, B20");
    Serial.println("│   o2:       B8, B20");
    Serial.println("│   1NCE/TM:  B3, B8, B20 (Roaming)");
    Serial.println("│");
    Serial.println("│ Tippe 'lte bands fix' um B3,B8,B20 zu setzen");
    Serial.println("│ Tippe 'lte bands all' um alle Baender freizuschalten");
    Serial.println("└──────────────────────────────────────┘");
}

void modem_lte_bands_fix(bool all) {
    if (all) {
        Serial.println("[MODEM] Setze alle Baender (CAT-M + NB-IoT)...");
        s_modem.sendAT("+CBANDCFG=\"CAT-M\",1,2,3,4,5,8,12,13,14,17,18,19,20,25,26,27,28,66,85");
        s_modem.waitResponse(3000L);
        s_modem.sendAT("+CBANDCFG=\"NB-IOT\",1,2,3,5,8,12,13,17,18,19,20,25,26,28,66,71,85");
        s_modem.waitResponse(3000L);
    } else {
        Serial.println("[MODEM] Setze DE-Baender B3,B8,B20 (CAT-M + NB-IoT)...");
        s_modem.sendAT("+CBANDCFG=\"CAT-M\",3,8,20");
        s_modem.waitResponse(3000L);
        s_modem.sendAT("+CBANDCFG=\"NB-IOT\",3,8,20");
        s_modem.waitResponse(3000L);
    }

    // Radio-Neustart damit Aenderung greift
    Serial.println("[MODEM] Radio-Neustart (CFUN=0/1)...");
    at_ok("+CFUN=0", 10000L);
    delay(2000);
    at_ok("+CFUN=1", 10000L);
    delay(5000);

    // SIM-PIN ggf. erneut eingeben nach CFUN-Reset
    if (!ensure_sim_ready()) {
        Serial.println("[MODEM] WARNUNG: SIM nach CFUN nicht READY");
    }

    // CNMP/CMNB NACH CFUN=1 neu setzen — CFUN=0/1 setzt auf Default zurueck
    at_ok("+CNMP=38");
    at_ok("+CMNB=1");

    Serial.println("[MODEM] Baender gesetzt + Radio neu gestartet");
    Serial.println("[MODEM] Netzsuche laeuft ueber Modem-Task (manuell)...");

    // Neue Config anzeigen
    modem_print_lte_bands();
}

// FIXES B.17: Watchdog-Reboot-Counter beim normalen Deep Sleep nullen. Er liegt
// in RTC_DATA und überlebt den Sleep sonst → die nächste Fahrt überspringt
// grundlos den Präferenz-Provider (try_register Phase 0 wird wegen
// s_wd_reboot_count != 0 ausgelassen).
void modem_clear_reboot_count() {
    s_wd_reboot_count       = 0;
    s_wd_first_reboot_epoch = 0;
}

void modem_pause_task() {
    if (s_task_paused) {
        Serial.println("[MODEM] Task bereits pausiert");
        return;
    }
    s_task_paused = true;
    // Task sofort hart suspenden — bricht laufende AT-Timeouts ab
    if (s_modem_task_handle) vTaskSuspend(s_modem_task_handle);
    // UART-Buffer leeren (halbe Responses vom unterbrochenen AT-Befehl)
    delay(100);
    while (s_serial.available()) s_serial.read();
    Serial.println("[MODEM] Task pausiert — 'at start' zum Fortsetzen");
    Serial.println("[MODEM] Hinweis: Falls COPS=? lief, antwortet Modem erst nach Scan-Ende (bis 3 Min)");
}

void modem_resume_task() {
    if (!s_task_paused) {
        Serial.println("[MODEM] Task laeuft bereits");
        return;
    }
    s_task_paused = false;
    if (s_modem_task_handle) vTaskResume(s_modem_task_handle);
    Serial.println("[MODEM] Task fortgesetzt");
}

void modem_send_at(const char* cmd) {
    // Modem-Task pausieren um Kollisionen zu vermeiden (nur wenn nicht schon pausiert)
    bool need_resume = !s_task_paused && s_modem_task_handle;
    if (need_resume) vTaskSuspend(s_modem_task_handle);

    // COPS-Befehle brauchen langen Timeout (Netzsuche bis 120s)
    long timeout = 10000L;
    if (strncmp(cmd, "+COPS", 5) == 0) timeout = 120000L;

    if (s_task_paused) {
        // Raw-Modus: UART-Buffer leeren, direkt senden, roh lesen
        // Verhindert TinyGSM-Verwirrung durch alte COPS=?-Responses
        while (s_serial.available()) s_serial.read();

        Serial.printf("[AT] → AT%s\n", cmd);
        s_serial.printf("AT%s\r\n", cmd);

        // Antwort zeilenweise lesen bis OK/ERROR oder Timeout
        uint32_t start = millis();
        String line;
        while (millis() - start < (uint32_t)timeout) {
            if (s_serial.available()) {
                char c = s_serial.read();
                if (c == '\n') {
                    line.trim();
                    if (line.length() > 0) {
                        Serial.printf("[AT] ← %s\n", line.c_str());
                    }
                    if (line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR")) {
                        break;
                    }
                    line = "";
                } else if (c != '\r') {
                    line += c;
                }
            } else {
                delay(10);
            }
        }
        if (millis() - start >= (uint32_t)timeout) {
            Serial.println("[AT] TIMEOUT");
        }
    } else {
        // TinyGSM-Modus (Task laeuft normal)
        Serial.printf("[AT] → %s (timeout %lds)\n", cmd, timeout / 1000);
        s_modem.sendAT(cmd);
        String resp = "";
        int r = s_modem.waitResponse(timeout, resp);
        resp.trim();
        if (resp.length() > 0) {
            Serial.printf("[AT] ← %s\n", resp.c_str());
        }
        Serial.printf("[AT] rc=%d (%s)\n", r, r == 1 ? "OK" : r == 2 ? "ERROR" : "TIMEOUT/OTHER");
    }

    if (need_resume) vTaskResume(s_modem_task_handle);
}

int  modem_gps_vsat()    { return s_gps_vsat; }
int  modem_gps_usat()    { return s_gps_usat; }

void modem_print_gps_info() {
    GpsSnapshot snap = gps_snapshot();
    Serial.println("┌─── GPS Info ─────────────────────────┐");
    Serial.printf( "│ Fix:        %s\n", s_gps_has_fix ? "JA" : "NEIN");
    if (snap.valid) {
        Serial.printf( "│ Position:   %.6f, %.6f\n", snap.lat, snap.lon);
    } else {
        Serial.println("│ Position:   ---");
    }
    Serial.printf( "│ Satelliten: %d/%d (verwendet/sichtbar)\n", s_gps_usat, s_gps_vsat);
    Serial.printf( "│ HDOP:       %.1f\n", s_gps_hdop);
    Serial.printf( "│ Höhe:       %.0f m\n", s_gps_alt);
    Serial.printf( "│ Speed:      %.1f km/h\n", s_gps_speed);
    Serial.println("└──────────────────────────────────────┘");
}
