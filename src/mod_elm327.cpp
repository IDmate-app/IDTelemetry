#include "mod_elm327.h"
#include "mod_sleep.h"
#include "mod_ble_scan.h"
#include "mod_can.h"
#include "mod_logs.h"
#include "mod_telemetry.h"
#include "mod_config.h"

// ELM TX/RX ins Log schreiben
static void elm_log_rx(uint32_t id, uint8_t* data, uint8_t len, const char* label) {
    log_add(false, id, data, len, label, LOG_SRC_ELM);
}
#include <NimBLEDevice.h>

// ============================================================
//  mod_elm327 - BLE ELM327 Emulation (NimBLE)
//
//  Von ABRP abgefragte VW-spezifische UDS DIDs (Mode 22):
//
//  ECU 0xFC007B (Motor/Antrieb) - ATSH FC007B, CRA 17FE007B:
//    221E3D  → SoC oder Reichweite (noch unbekannt)
//    221E3B  → SoC oder Reichweite (noch unbekannt)
//    22028C  → Batteriespannung?
//    227448  → Temperatur?
//    222A0B  → Strom?
//    22F40D  → unbekannt
//    221E32  → unbekannt
//
//  ECU 0xFC0076 (Batterie) - ATSH FC0076, CRA 17FE0076:
//    22210E  → unbekannt
//    22295A  → unbekannt
//    220364  → unbekannt
//
//  ECU 0x710 (BMS?) - ATSH 710, CRA 77A:
//    222AB2  → unbekannt
//
//  ECU 0x746 - ATSH 746, CRA 7B0:
//    222613  → unbekannt
//    222609  → unbekannt
//
//  Sobald CAN-Bus angeschlossen: rohe Antwort-Bytes loggen
//  und mit ABRP-Anzeige abgleichen um Dekodierung zu ermitteln.
// ============================================================
//
//  Kompatibel mit: ABRP, Torque Pro, OBD Fusion, Car Scanner
// ============================================================

// ---- ELM327 Interne State ----
static bool elm_echo       = true;   // ATE1/ATE0
static bool elm_spaces     = true;   // ATS1/ATS0
static bool elm_headers    = false;  // ATH1/ATH0
static bool elm_linefeed   = true;   // ATL1/ATL0
static uint8_t elm_proto   = 6;      // ATSP (6 = ISO 15765-4 CAN 500kbps)
static bool    elm_caf     = true;   // ATCAF1/ATCAF0 (CAN Auto Formatting)
static uint32_t elm_timeout= 300;    // ATST

// Standard OBD2 Header (7DF Broadcast)
static uint32_t elm_req_id  = 0x7DF;
static uint32_t elm_resp_id = 0;     // 0 = alle akzeptieren
static uint8_t  elm_can_prio = 0x18; // ATCP: CAN Priority (obere 5 Bits bei 29-bit)

// ---- Flow Control (ISO-TP Multi-Frame) ----
static uint32_t elm_fc_header = 0;       // ATFCSH: Flow Control TX-ID (0 = elm_req_id verwenden)
static uint8_t  elm_fc_data[8] = {0x30, 0x00, 0x00, 0,0,0,0,0};  // ATFCSD: FC-Frame Daten
static uint8_t  elm_fc_data_len = 3;     // Länge der FC-Daten
static uint8_t  elm_fc_mode = 0;         // ATFCSM: 0=default, 1=user-defined header+data

// ---- BLE ----
static NimBLEServer*          pServer    = nullptr;
static NimBLECharacteristic*  pCharWrite = nullptr;
static NimBLECharacteristic*  pCharNotify= nullptr;
static bool                   ble_connected = false;

// ---- Command Buffer ----
static char cmd_buf[64];
static int  cmd_len = 0;
static volatile bool send_greeting = false; // gesetzt in Callback, gesendet im Loop

// ---- Command Queue: BLE-Callback → eigener Task ----
static QueueHandle_t s_cmd_queue = NULL;

// ============================================================
//  Hilfsfunktionen
// ============================================================
static void elm_send(const char* msg) {
    if (!pCharNotify || !ble_connected) return;
    size_t total = strlen(msg);
    // MTU: NimBLE verhandelt automatisch, aber Notify-Payload max MTU-3
    // Sicher: 20 Bytes pro Paket (BLE 4.0 Minimum)
    uint16_t mtu = 0;
    if (pServer) {
        auto peers = pServer->getPeerDevices();
        if (!peers.empty()) mtu = pServer->getPeerMTU(peers[0]);
    }
    uint16_t chunk = (mtu > 3) ? (mtu - 3) : 20;
    if (chunk > 240) chunk = 240;  // Sicherheitslimit

    const uint8_t* p = (const uint8_t*)msg;
    while (total > 0) {
        uint16_t n = (total > chunk) ? chunk : (uint16_t)total;
        pCharNotify->setValue(p, n);
        pCharNotify->notify();
        p     += n;
        total -= n;
        if (total > 0) vTaskDelay(pdMS_TO_TICKS(10)); // BLE-Stack Zeit geben
    }
}

static void elm_prompt() {
    elm_send(">");
}

static void elm_ok() {
    elm_send(elm_linefeed ? "OK\r\n" : "OK\r");
    elm_prompt();
}

// Hex String aus CAN Bytes bauen
static String can_bytes_to_elm(uint8_t* data, uint8_t len, uint32_t id) {
    String out = "";
    if (elm_headers) {
        char h[16]; snprintf(h, sizeof(h), "%03X", id);
        out += h;
        if (elm_spaces) out += " ";
    }
    for (int i = 0; i < len; i++) {
        char b[4]; snprintf(b, sizeof(b), elm_spaces ? "%02X " : "%02X", data[i]);
        out += b;
    }
    out.trim();
    return out;
}

// Forward Declarations (definiert nach DID_CACHE_MAP)
static int isotp_reassemble(uint8_t frames[][8], uint8_t lens[], int frame_count,
                            uint8_t* pdu, int max_pdu);
static String pdu_to_hex(const uint8_t* pdu, int len);

// ============================================================
//  OBD2 Standard PID Berechnung (Mode 01)
// ============================================================
static bool handle_obd2_pid(uint8_t pid, String& response) {
    uint8_t req_data[2] = { 0x01, pid };
    char label[32]; snprintf(label, sizeof(label), "ELM OBD2 PID=0x%02X", pid);

    if (!can_lock(1500)) { response = "NO DATA"; return false; }

    uint32_t fc_id = (elm_fc_mode == 1 && elm_fc_header != 0) ? elm_fc_header : 0;
    uint8_t* fc_data_ptr = (elm_fc_mode == 1) ? elm_fc_data : nullptr;
    uint8_t  fc_len      = (elm_fc_mode == 1) ? elm_fc_data_len : 0;

    uint8_t frames[8][8]; uint8_t lens[8]; uint32_t ids[8];
    int n = can_isotp_query_elm(elm_req_id, elm_resp_id,
                                fc_id, fc_data_ptr, fc_len,
                                req_data, 2,
                                frames, lens, ids, 8,
                                elm_timeout + 200);
    can_unlock();

    if (n == 0) {
        uint8_t _nd[1]={0}; log_add(false, elm_req_id, _nd, 0, "NO DATA", LOG_SRC_ELM);
        response = "NO DATA";
        return false;
    }

    for (int i = 0; i < n; i++)
        elm_log_rx(ids[i], frames[i], lens[i], label);

    if (elm_caf) {
        uint8_t pdu[64];
        int pdu_len = isotp_reassemble(frames, lens, n, pdu, sizeof(pdu));
        response = pdu_to_hex(pdu, pdu_len);
    } else {
        response = "";
        for (int i = 0; i < n; i++) {
            response += can_bytes_to_elm(frames[i], lens[i], ids[i]);
            if (i < n-1) response += "\r";
        }
    }
    return true;
}

// ============================================================
//  ABRP Cache-first: DID → Telemetrie-Cache Mapping
//  ABRP-Anfragen werden aus dem Telemetrie-Cache beantwortet
//  solange der Wert frisch genug ist (kein CAN-Traffic dadurch).
// ============================================================
struct DidCacheEntry {
    uint16_t   did;
    TelemField field;
    uint32_t   max_age_ms;  // Polling-Intervall + 20% Puffer
};

static const DidCacheEntry DID_CACHE_MAP[] = {
    { 0x028C, TELEM_SOC,           6000  },
    { 0x1E3B, TELEM_VOLTAGE,       6000  },
    { 0x1E3D, TELEM_CURRENT,       6000  },
    { 0xF40D, TELEM_VEHICLE_SPEED, 6000  },
    { 0x7448, TELEM_IS_CHARGING,   6000  },
    { 0x2A0B, TELEM_BATT_TEMP,     12000 },
    { 0x2609, TELEM_EXT_TEMP,      12000 },
    { 0x2AB2, TELEM_CAPACITY,      36000 },
    { 0x1E32, TELEM_KWH_CHARGED,   36000 },
    { 0x210E, TELEM_IS_PARKED,     36000 },
    { 0x295A, TELEM_ODOMETER,      72000 },
};

// Float-Wert → synthetische UDS-Antwort-Bytes (Rückweg der Dekodierformel)
// Baut vollständige UDS-Antwort: 62 + DID_H + DID_L + Datenbytes
static bool elm_cache_response(uint16_t did, float val, String& out) {
    uint8_t b[16] = {};
    uint8_t len   = 0;

    switch (did) {
        case 0x028C: // SoC: A = (v + 6.1947) / 0.4425
            b[0] = (uint8_t)((val + 6.1947f) / 0.4425f + 0.5f);
            len = 1; break;
        case 0x1E3B: { // Spannung: A:B = v * 4
            uint16_t raw = (uint16_t)(val * 4.f + 0.5f);
            b[0] = (uint8_t)(raw >> 8);
            b[1] = (uint8_t)(raw & 0xFF);
            len = 2; break;
        }
        case 0x1E3D: { // Strom: INT32 = (-v * 100) + 150000
            int32_t raw = (int32_t)(-val * 100.f + 150000.f);
            b[0] = (uint8_t)((raw >> 24) & 0xFF);
            b[1] = (uint8_t)((raw >> 16) & 0xFF);
            b[2] = (uint8_t)((raw >>  8) & 0xFF);
            b[3] = (uint8_t)( raw        & 0xFF);
            len = 4; break;
        }
        case 0xF40D: // Geschwindigkeit: A = v
            b[0] = (uint8_t)(val + 0.5f);
            len = 1; break;
        case 0x7448: { // Ladestatus: Bit 2 = charging, Bit 1+2 = dcfc
            float dcfc_val = 0.0f; uint32_t dcfc_age = 0;
            telem_get_latest(TELEM_IS_DCFC, &dcfc_val, &dcfc_age);
            b[0] = (val > 0.5f ? 0x04 : 0x00) | (dcfc_val > 0.5f ? 0x02 : 0x00);
            len = 1; break;
        }
        case 0x2A0B: // batt_temp: A = (v + 40) * 2
            b[0] = (uint8_t)((val + 40.f) * 2.f + 0.5f);
            len = 1; break;
        case 0x2609: // ext_temp: A = (v + 50) * 2
            b[0] = (uint8_t)((val + 50.f) * 2.f + 0.5f);
            len = 1; break;
        case 0x2AB2: { // Kapazität: INT16 = val * 1000 / 50
            int16_t raw = (int16_t)(val * 1000.f / 50.f + 0.5f);
            b[0] = (uint8_t)((raw >> 8) & 0xFF);
            b[1] = (uint8_t)(raw & 0xFF);
            len = 2; break;
        }
        case 0x1E32: { // Geladen kWh: 12 Bytes, Bytes 8-11 = raw = val * 8583.07
            memset(b, 0, 12);
            int32_t raw = (int32_t)(val * 8583.07123641215f);
            b[8]  = (uint8_t)((raw >> 24) & 0xFF);
            b[9]  = (uint8_t)((raw >> 16) & 0xFF);
            b[10] = (uint8_t)((raw >>  8) & 0xFF);
            b[11] = (uint8_t)( raw        & 0xFF);
            len = 12; break;
        }
        case 0x210E: { // Geparkt: 2 Bytes, B == 8 → parked
            b[0] = 0x00;
            b[1] = (val > 0.5f) ? 0x08 : 0x00;
            len = 2; break;
        }
        case 0x295A: { // Odometer: 3 Bytes, 24-bit uint
            uint32_t raw = (uint32_t)(val + 0.5f);
            b[0] = (uint8_t)((raw >> 16) & 0xFF);
            b[1] = (uint8_t)((raw >>  8) & 0xFF);
            b[2] = (uint8_t)( raw        & 0xFF);
            len = 3; break;
        }
        default:
            return false;
    }

    // UDS-Antwort-Header: 62 + DID_H + DID_L + Datenbytes
    out = "";
    char hdr[12];
    snprintf(hdr, sizeof(hdr), elm_spaces ? "62 %02X %02X " : "62%02X%02X",
             (uint8_t)(did >> 8), (uint8_t)(did & 0xFF));
    out += hdr;
    for (int i = 0; i < len; i++) {
        char h[4];
        snprintf(h, sizeof(h), elm_spaces ? "%02X " : "%02X", b[i]);
        out += h;
    }
    out.trim();
    return true;
}

// ── ISO-TP Reassembly: rohe CAN-Frames → UDS-PDU ──────────
// Gibt Byte-Anzahl der PDU zurück, oder 0 bei Fehler
static int isotp_reassemble(uint8_t frames[][8], uint8_t lens[], int frame_count,
                            uint8_t* pdu, int max_pdu) {
    int pdu_len = 0;
    int total_len = 0;  // ISO-TP Gesamtlänge (aus FF)
    for (int i = 0; i < frame_count && pdu_len < max_pdu; i++) {
        uint8_t pci = frames[i][0] >> 4;
        if (pci == 0 && i == 0) {
            int sf_len = frames[i][0] & 0x0F;
            if (sf_len > lens[i] - 1) sf_len = lens[i] - 1;
            if (sf_len > max_pdu) sf_len = max_pdu;
            memcpy(pdu, frames[i] + 1, sf_len);
            pdu_len = sf_len;
            total_len = sf_len;
        } else if (pci == 1 && i == 0) {
            total_len = ((frames[i][0] & 0x0F) << 8) | frames[i][1];
            int ff_data = lens[i] - 2;
            if (ff_data > max_pdu) ff_data = max_pdu;
            if (ff_data > 0) { memcpy(pdu, frames[i] + 2, ff_data); pdu_len = ff_data; }
        } else if (pci == 2) {
            int cf_data = lens[i] - 1;
            if (pdu_len + cf_data > max_pdu) cf_data = max_pdu - pdu_len;
            memcpy(pdu + pdu_len, frames[i] + 1, cf_data);
            pdu_len += cf_data;
        }
    }
    // Auf tatsächliche Gesamtlänge trimmen (Padding aus letztem CF entfernen)
    if (total_len > 0 && pdu_len > total_len)
        pdu_len = total_len;
    return pdu_len;
}

// ── PDU als Hex-String formatieren (CAF1 Antwort) ──────────
static String pdu_to_hex(const uint8_t* pdu, int len) {
    String out = "";
    for (int i = 0; i < len; i++) {
        char b[4]; snprintf(b, sizeof(b), elm_spaces ? "%02X " : "%02X", pdu[i]);
        out += b;
    }
    out.trim();
    return out;
}

// ============================================================
//  UDS Service 0x22 via ELM (für ABRP VW-spezifisch)
//  Format: "22XXYY" → UDS Read DID 0xXXYY
// ============================================================
// Standby-Defaults: plausible Standwerte wenn CAN-Cache noch leer
// (Fahrzeug aus, Boot-Phase, kein CAN). ABRP bekommt immer eine Antwort.
static float did_standby_value(uint16_t did) {
    switch (did) {
        case 0x028C: return 80.0f;   // SoC 80%
        case 0x1E3B: return 390.0f;  // Spannung ~390V (Ruhe bei ~80%)
        case 0x1E3D: return 0.0f;    // Strom 0A (Stand)
        case 0xF40D: return 0.0f;    // Speed 0 km/h
        case 0x7448: return 0.0f;    // nicht ladend
        case 0x2A0B: return 20.0f;   // Batt-Temp 20°C
        case 0x2609: return 15.0f;   // Außen-Temp 15°C
        case 0x2AB2: return 86.5f;   // Kapazität 86.5 kWh (ID.7)
        case 0x1E32: return 0.0f;    // kWh geladen 0
        case 0x210E: return 1.0f;    // Geparkt: ja
        case 0x295A: return 404.0f;  // Odometer 404 km
        default:     return 0.0f;
    }
}

static bool handle_uds_did(uint16_t did, String& response) {
    // Bekannte DIDs: IMMER aus Telemetrie-Cache antworten (kein CAN-Traffic).
    // telem_task pollt den Bus selbstständig — ABRP bekommt nur Cache-Daten.
    // Falls Cache leer (Fahrzeug aus, Boot): Standby-Defaults zurückgeben.
    for (size_t i = 0; i < sizeof(DID_CACHE_MAP)/sizeof(DID_CACHE_MAP[0]); i++) {
        if (DID_CACHE_MAP[i].did != did) continue;
        float val; uint32_t age_ms;
        bool valid = telem_get_latest(DID_CACHE_MAP[i].field, &val, &age_ms);
        if (!valid) {
            if (!cfg_ble_standby()) {
                response = "NO DATA";
                return false;
            }
            val = did_standby_value(did);
        }
        String cached;
        if (elm_cache_response(did, val, cached)) {
            response = cached;
            return true;
        }
        response = "NO DATA";
        return false;
    }

    // Unbekannte DID (z.B. 0x0364, 0x2613): CAN-Fallback für Kompatibilität
    uint8_t req_data[3] = { 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };

    if (!can_lock(1500)) { response = "NO DATA"; return false; }

    uint32_t fc_id = (elm_fc_mode == 1 && elm_fc_header != 0) ? elm_fc_header : 0;
    uint8_t* fc_data_ptr = (elm_fc_mode == 1) ? elm_fc_data : nullptr;
    uint8_t  fc_len      = (elm_fc_mode == 1) ? elm_fc_data_len : 0;

    uint8_t frames[16][8]; uint8_t lens[16]; uint32_t ids[16];
    int n = can_isotp_query_elm(elm_req_id, elm_resp_id,
                                fc_id, fc_data_ptr, fc_len,
                                req_data, 3,
                                frames, lens, ids, 16,
                                elm_timeout + 200);
    can_unlock();

    if (n == 0) {
        uint8_t _nd[1]={0}; log_add(false, elm_req_id, _nd, 0, "NO DATA", LOG_SRC_ELM);
        response = "NO DATA";
        return false;
    }

    char label[32]; snprintf(label, sizeof(label), "ELM UDS DID=0x%04X", did);
    for (int i = 0; i < n; i++)
        elm_log_rx(ids[i], frames[i], lens[i], label);

    uint8_t pdu[128];
    int pdu_len = isotp_reassemble(frames, lens, n, pdu, sizeof(pdu));

    if (pdu_len >= 4 && pdu[0] == 0x62)
        telem_feed_did(did, pdu + 3, pdu_len - 3);

    if (elm_caf && pdu_len > 0) {
        response = pdu_to_hex(pdu, pdu_len);
    } else {
        response = "";
        for (int i = 0; i < n; i++) {
            response += can_bytes_to_elm(frames[i], lens[i], ids[i]);
            if (i < n-1) response += "\r";
        }
    }

    return true;
}

// ============================================================
//  AT Befehl verarbeiten
// ============================================================
static void process_at(const char* cmd) {
    // ATZ - Reset
    if (strcmp(cmd, "Z") == 0) {
        elm_echo    = true;
        elm_spaces  = true;
        elm_headers = false;
        elm_linefeed= true;
        elm_proto   = 6;
        elm_caf     = true;
        elm_timeout = 300;
        elm_req_id  = 0x7DF;
        elm_resp_id = 0;
        elm_can_prio = 0x18;
        elm_fc_header = 0;
        elm_fc_data[0] = 0x30; elm_fc_data[1] = 0x00; elm_fc_data[2] = 0x00;
        elm_fc_data_len = 3;
        elm_fc_mode = 0;
        elm_send(elm_linefeed ? "\r\nELM327 v1.5\r\n" : "\rELM327 v1.5\r");
        elm_prompt();
        return;
    }

    // ATE - Echo
    if (cmd[0] == 'E') {
        elm_echo = (cmd[1] != '0');
        elm_ok(); return;
    }

    // ATL - Linefeed
    if (cmd[0] == 'L') {
        elm_linefeed = (cmd[1] != '0');
        elm_ok(); return;
    }

    // ATS - Spaces
    if (cmd[0] == 'S' && (cmd[1] == '0' || cmd[1] == '1')) {
        elm_spaces = (cmd[1] != '0');
        elm_ok(); return;
    }

    // ATH - Headers
    if (cmd[0] == 'H') {
        elm_headers = (cmd[1] == '1');
        elm_ok(); return;
    }

    // ATSP - Set Protocol
    if (cmd[0] == 'S' && cmd[1] == 'P') {
        elm_proto = strtoul(cmd + 2, nullptr, 16);
        elm_ok(); return;
    }

    // ATST - Set Timeout (in 4ms units)
    if (cmd[0] == 'S' && cmd[1] == 'T') {
        uint8_t t = strtoul(cmd + 2, nullptr, 16);
        elm_timeout = t * 4;
        elm_ok(); return;
    }

    // ATAT - Adaptive Timing
    if (cmd[0] == 'A' && cmd[1] == 'T') {
        elm_ok(); return;  // ignorieren, OK zurück
    }

    // ATSH - Set Header (CAN Request ID)
    // 3 hex chars = 11-bit CAN ID, >3 hex chars = 29-bit (ATCP Priority dazurechnen)
    if (cmd[0] == 'S' && cmd[1] == 'H') {
        const char* hex = cmd + 2;
        int hex_len = strlen(hex);
        uint32_t raw_id = strtoul(hex, nullptr, 16);

        if (hex_len > 3) {
            // 29-bit: ATCP-Priority (obere 5 Bits) + ATSH (untere 24 Bits)
            elm_req_id = ((uint32_t)elm_can_prio << 24) | (raw_id & 0x00FFFFFF);
            // 29-bit Header gesetzt
        } else {
            // 11-bit: direkt verwenden
            elm_req_id = raw_id & 0x7FF;
        }

        // Response ID automatisch ableiten (VAG Schema)
        if ((elm_req_id & 0x7FF) <= 0x7FF && hex_len <= 3) {
            if ((elm_req_id & 0xFF0) == 0x7E0)
                elm_resp_id = elm_req_id + 8;
            else if (elm_req_id == 0x7DF)
                elm_resp_id = 0;
            else
                elm_resp_id = 0;
        } else {
            elm_resp_id = 0;  // 29-bit: ATCRA setzt den Filter
        }
        elm_ok(); return;
    }

    // ATCRA - Set CAN Receive Address (Response Filter)
    if (cmd[0] == 'C' && cmd[1] == 'R' && cmd[2] == 'A') {
        elm_resp_id = strtoul(cmd + 3, nullptr, 16);
        elm_ok(); return;
    }

    // ATCAF - CAN Auto Format (1=reassemble ISO-TP, strip PCI+padding)
    if (cmd[0] == 'C' && cmd[1] == 'A' && cmd[2] == 'F') {
        elm_caf = (cmd[3] != '0');
        elm_ok(); return;
    }

    // ATDP - Describe Protocol
    if (strcmp(cmd, "DP") == 0) {
        elm_send("ISO 15765-4 (CAN 11/500)\r\n>");
        return;
    }

    // ATDPN - Describe Protocol Number
    if (strcmp(cmd, "DPN") == 0) {
        char r[8]; snprintf(r, sizeof(r), "6\r\n>");
        elm_send(r); return;
    }

    // ATI - Identify
    if (strcmp(cmd, "I") == 0) {
        elm_send("ELM327 v1.5\r\n>");
        return;
    }

    // ATRV - Read Voltage
    if (strcmp(cmd, "RV") == 0) {
        elm_send("12.4V\r\n>");
        return;
    }

    // ATIGN - Ignition Status
    if (strcmp(cmd, "IGN") == 0) {
        elm_send("ON\r\n>");
        return;
    }

    // ATWS - Warm Start
    if (strcmp(cmd, "WS") == 0) {
        elm_send("ELM327 v1.5\r\n>");
        elm_prompt();
        return;
    }

    // ATRR - Reset Results
    if (strcmp(cmd, "RR") == 0) { elm_ok(); return; }

    // ATCP - CAN Priority (obere 5 Bits bei 29-bit CAN IDs)
    if (cmd[0] == 'C' && cmd[1] == 'P') {
        elm_can_prio = (uint8_t)strtoul(cmd + 2, nullptr, 16);
        elm_ok(); return;
    }

    // ATFCSH - Flow Control Set Header (TX-ID für FC-Frame)
    if (cmd[0] == 'F' && cmd[1] == 'C' && cmd[2] == 'S' && cmd[3] == 'H') {
        elm_fc_header = strtoul(cmd + 4, nullptr, 16);
        elm_ok(); return;
    }
    // ATFCSD - Flow Control Set Data (Hex-Bytes für FC-Frame)
    if (cmd[0] == 'F' && cmd[1] == 'C' && cmd[2] == 'S' && cmd[3] == 'D') {
        const char* p = cmd + 4;
        elm_fc_data_len = 0;
        while (*p && elm_fc_data_len < 8) {
            char hex[3] = { p[0], p[1] ? p[1] : '0', 0 };
            elm_fc_data[elm_fc_data_len++] = (uint8_t)strtoul(hex, nullptr, 16);
            p += (p[1]) ? 2 : 1;
        }
        elm_ok(); return;
    }
    // ATFCSM - Flow Control Set Mode (0=default, 1=user-defined)
    if (cmd[0] == 'F' && cmd[1] == 'C' && cmd[2] == 'S' && cmd[3] == 'M') {
        elm_fc_mode = (uint8_t)strtoul(cmd + 4, nullptr, 10);
        elm_ok(); return;
    }

    // ATAR / ATARTA - Auto Receive
    if (strncmp(cmd, "AR", 2) == 0) { elm_ok(); return; }

    // ATM0 / ATM1 - Memory
    if (cmd[0] == 'M' && (cmd[1] == '0' || cmd[1] == '1')) { elm_ok(); return; }

    // ATAT0/1/2 - Adaptive Timing
    if (cmd[0] == 'A' && cmd[1] == 'T') { elm_ok(); return; }

    // Unbekannt
    Serial.printf("[ELM AT] Unbekannt: %s\n", cmd);
    elm_send("?\r\n>");
}

// ============================================================
//  OBD/UDS Befehl verarbeiten (keine AT-Prefix)
// ============================================================
static void process_obd(const char* cmd) {
    // OBD-Polling stumm — nur Fehler werden geloggt

#ifdef ELM_MOCK_MODE
    // Mock: alle OBD-Anfragen mit gueltigen Dummy-Werten beantworten
    {
        int len = strlen(cmd);
        char mode_s[3] = {cmd[0], cmd[1], 0};
        uint8_t mode = strtoul(mode_s, nullptr, 16);
        char pid_s[3]  = {len>=4?cmd[2]:'0', len>=4?cmd[3]:'0', 0};
        uint8_t pid  = strtoul(pid_s,  nullptr, 16);
        char mock_resp[32];
        if (mode == 0x01) {
            snprintf(mock_resp, sizeof(mock_resp), "%02X %02X 00 00 00 00", mode+0x40, pid);
        } else if (mode == 0x22) {
            snprintf(mock_resp, sizeof(mock_resp), "62 %02X %02X 00 00", (uint8_t)(strtoul(cmd+2,nullptr,16)>>8), (uint8_t)(strtoul(cmd+2,nullptr,16)&0xFF));
        } else {
            snprintf(mock_resp, sizeof(mock_resp), "NO DATA");
        }
        String out = "";
        if (elm_echo) { out += cmd; out += "\r"; }
        out += mock_resp;
        out += elm_linefeed ? "\r\n>" : "\r>";
        elm_send(out.c_str());
        return;
    }
#endif

    int len = strlen(cmd);
    if (len < 2) { elm_send("?\r\n>"); return; }

    // Mode und Bytes parsen
    char mode_s[3] = {cmd[0], cmd[1], 0};
    uint8_t mode = strtoul(mode_s, nullptr, 16);

    String response;

    // Mode 01 - OBD2 Standard PIDs
    if (mode == 0x01 && len >= 4) {
        char pid_s[3] = {cmd[2], cmd[3], 0};
        uint8_t pid = strtoul(pid_s, nullptr, 16);
        handle_obd2_pid(pid, response);
    }
    // Mode 09 - Vehicle Info (VIN etc.)
    else if (mode == 0x09 && len >= 4) {
        char pid_s[3] = {cmd[2], cmd[3], 0};
        uint8_t pid = strtoul(pid_s, nullptr, 16);
        uint8_t req_data[2] = { 0x09, pid };
        char label[32]; snprintf(label, sizeof(label), "ELM Mode09 PID=0x%02X", pid);

        uint32_t fc_id = (elm_fc_mode == 1 && elm_fc_header != 0) ? elm_fc_header : 0;
        uint8_t frames[8][8]; uint8_t lens[8]; uint32_t ids[8];
        int n = 0;
        if (can_lock()) {
            n = can_isotp_query_elm(elm_req_id, elm_resp_id,
                                    fc_id, elm_fc_mode == 1 ? elm_fc_data : nullptr,
                                    elm_fc_mode == 1 ? elm_fc_data_len : 0,
                                    req_data, 2, frames, lens, ids, 8,
                                    elm_timeout + 200);
            can_unlock();
        }
        if (n == 0) { uint8_t _nd[1]={0}; log_add(false, elm_req_id, _nd, 0, "NO DATA", LOG_SRC_ELM); response = "NO DATA"; }
        else {
            for (int i = 0; i < n; i++)
                elm_log_rx(ids[i], frames[i], lens[i], label);
            if (elm_caf) {
                uint8_t pdu[64];
                int pdu_len = isotp_reassemble(frames, lens, n, pdu, sizeof(pdu));
                response = pdu_to_hex(pdu, pdu_len);
            } else {
                for (int i = 0; i < n; i++) {
                    response += can_bytes_to_elm(frames[i], lens[i], ids[i]);
                    if (i < n-1) response += "\r";
                }
            }
        }
    }
    // Mode 22 - UDS Read DID (VAG-spezifisch, ABRP nutzt das für VW!)
    else if (mode == 0x22 && len >= 6) {
        char did_s[5] = {cmd[2], cmd[3], cmd[4], cmd[5], 0};
        uint16_t did = strtoul(did_s, nullptr, 16);
        handle_uds_did(did, response);
    }
    // Mode 03 - Read DTCs
    else if (mode == 0x03) {
        uint8_t req_data[1] = { 0x03 };
        uint32_t fc_id = (elm_fc_mode == 1 && elm_fc_header != 0) ? elm_fc_header : 0;
        uint8_t frames[8][8]; uint8_t lens[8]; uint32_t ids[8];
        int n = 0;
        if (can_lock()) {
            n = can_isotp_query_elm(elm_req_id, elm_resp_id,
                                    fc_id, elm_fc_mode == 1 ? elm_fc_data : nullptr,
                                    elm_fc_mode == 1 ? elm_fc_data_len : 0,
                                    req_data, 1, frames, lens, ids, 8,
                                    elm_timeout + 200);
            can_unlock();
        }
        if (n == 0) { uint8_t _nd[1]={0}; log_add(false, elm_req_id, _nd, 0, "NO DATA", LOG_SRC_ELM); response = "NO DATA"; }
        else {
            for (int i = 0; i < n; i++)
                elm_log_rx(ids[i], frames[i], lens[i], "ELM ReadDTC");
            if (elm_caf) {
                uint8_t pdu[64];
                int pdu_len = isotp_reassemble(frames, lens, n, pdu, sizeof(pdu));
                response = pdu_to_hex(pdu, pdu_len);
            } else {
                for (int i = 0; i < n; i++) {
                    response += can_bytes_to_elm(frames[i], lens[i], ids[i]);
                    if (i < n-1) response += "\r";
                }
            }
        }
    }
    else {
        response = "NO DATA";
    }

    // Antwort senden
    String out = "";
    if (elm_echo) { out += cmd; out += "\r"; }
    out += response;
    out += elm_linefeed ? "\r\n>" : "\r>";
    elm_send(out.c_str());
}

// ============================================================
//  Vollständigen Befehl verarbeiten
// ============================================================
static void process_command(const char* raw) {
    // Kopie, Whitespace trimmen, Uppercase
    char cmd[64];
    int j = 0;
    for (int i = 0; raw[i] && j < 63; i++) {
        if (raw[i] != ' ' && raw[i] != '\t')
            cmd[j++] = toupper(raw[i]);
    }
    cmd[j] = '\0';

    if (strlen(cmd) == 0) { elm_prompt(); return; }

    // AT Befehl?
    if (cmd[0] == 'A' && cmd[1] == 'T') {
        process_at(cmd + 2);
    } else {
        process_obd(cmd);
    }
}

// ============================================================
//  BLE Callbacks
// ============================================================
static const char* JSON_BLE_CONN   = "{\"type\":\"ble_status\",\"connected\":1}";
static const char* JSON_BLE_DISCONN = "{\"type\":\"ble_status\",\"connected\":0}";

// Verbindungs-Statistik fuer BLE-Diagnose (zwischen on/onDisconnect erhalten)
static uint32_t s_ble_conn_start_ms = 0;
static uint8_t  s_ble_peer_mac[6]   = {0};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s) override {
        ble_connected = true;
        cmd_len = 0;
        if (s_cmd_queue) {
            char junk[64];
            while (xQueueReceive(s_cmd_queue, junk, 0) == pdTRUE) {}
        }
        syslog("BLE", "App verbunden (ABRP/Torque)");
        extern void ws_broadcast_json(const char*);
        ws_broadcast_json(JSON_BLE_CONN);
    }
    // Variante mit Connection-Descriptor — loggt Peer-MAC + Verbindungsparameter
    void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
        s_ble_conn_start_ms = millis();
        memcpy(s_ble_peer_mac, desc->peer_ota_addr.val, 6);
        // conn_itvl: Einheit 1.25 ms; supervision_timeout: Einheit 10 ms
        uint16_t itvl_ms = (uint16_t)(desc->conn_itvl * 1.25f);
        uint16_t sup_ms  = (uint16_t)(desc->supervision_timeout * 10);
        int n_conn = s->getConnectedCount();
        char msg[128];
        snprintf(msg, sizeof(msg),
            "BLE Peer %02X:%02X:%02X:%02X:%02X:%02X itvl=%ums sup=%ums lat=%u conns=%d",
            s_ble_peer_mac[5], s_ble_peer_mac[4], s_ble_peer_mac[3],
            s_ble_peer_mac[2], s_ble_peer_mac[1], s_ble_peer_mac[0],
            itvl_ms, sup_ms, desc->conn_latency, n_conn);
        syslog("BLE", msg);
    }
    void onDisconnect(NimBLEServer* s) override {
        ble_connected = false;
        if (s_cmd_queue) {
            char junk[64];
            while (xQueueReceive(s_cmd_queue, junk, 0) == pdTRUE) {}
        }
        // Uptime seit Connect — wenn <2s: wahrscheinlich Stack-Churn oder Auth-Fehler
        uint32_t uptime_ms = (s_ble_conn_start_ms > 0) ? (millis() - s_ble_conn_start_ms) : 0;
        int n_conn = s->getConnectedCount();
        char msg[96];
        if (uptime_ms < 2000) {
            snprintf(msg, sizeof(msg), "App getrennt · kurz (%lums) conns=%d %s",
                (unsigned long)uptime_ms, n_conn,
                (uptime_ms < 500) ? "· FLAP?" : "");
        } else {
            snprintf(msg, sizeof(msg), "App getrennt · uptime %lus conns=%d",
                (unsigned long)(uptime_ms / 1000), n_conn);
        }
        syslog("BLE", msg);
        s_ble_conn_start_ms = 0;
        extern void ws_broadcast_json(const char*);
        ws_broadcast_json(JSON_BLE_DISCONN);
        NimBLEDevice::startAdvertising();
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* c, ble_gap_conn_desc* desc, uint16_t subValue) override {
        if (subValue > 0) {
            send_greeting = true; // im Loop senden, nicht im Callback
        }
    }
    void onWrite(NimBLECharacteristic* c) override {
        std::string val = c->getValue();
        for (char ch : val) {
            if (ch == '\r' || ch == '\n') {
                if (cmd_len > 0) {
                    cmd_buf[cmd_len] = '\0';
                    // Befehl in Queue → eigener Task verarbeitet, NimBLE-Stack wird nicht blockiert
                    if (s_cmd_queue) {
                        char qbuf[64];
                        memcpy(qbuf, cmd_buf, cmd_len + 1);
                        xQueueSend(s_cmd_queue, qbuf, 0); // non-blocking
                    }
                    cmd_len = 0;
                }
            } else if (cmd_len < 63) {
                cmd_buf[cmd_len++] = ch;
            }
        }
    }
};

static ServerCallbacks serverCB;
static CharCallbacks   charCB;

// ============================================================
//  Init
// ============================================================
void elm327_init() {
    // NimBLE wurde bereits in ble_scan_init() gestartet
    // Server + Service hier hinzufügen

    // Server von ble_scan_init holen
    pServer = ble_get_server();
    if (pServer == nullptr) {
        Serial.println("[ELM] FEHLER: BLE Server ist NULL! ble_scan_init() zuerst aufrufen!");
        return;
    }
    pServer->setCallbacks(&serverCB);
    NimBLEService* pService = pServer->createService("FFE0");
    // FFE1: Read + Write + Notify - alles in einer Charakteristik
    pCharNotify = pService->createCharacteristic(
        "FFE1",
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::NOTIFY
    );
    pCharNotify->setCallbacks(&charCB);
    pCharWrite = pCharNotify;
    pService->start();
    // Advertising starten
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID("FFE0");
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    NimBLEDevice::startAdvertising();

    { char m[48]; snprintf(m, sizeof(m), "Service FFE0 bereit: \"%s\"", ELM327_BLE_NAME); syslog("ELM", m); }

    // Command Queue + Worker-Task: verarbeitet BLE-Befehle außerhalb des NimBLE-Callbacks
    // Queue 8 statt 4: bei 500ms live-CAN pro Query reichen 4 Slots für ~2s Puffer nicht aus.
    s_cmd_queue = xQueueCreate(8, 64);

    // Core 1: NimBLE läuft auf Core 0. elm_worker auf Core 0 würde während 500ms CAN-Wartezeit
    // die NimBLE-Supervision stören und BLE-Verbindungsabbrüche verursachen.
    // Stack 6144: frames[16][8]+lens+ids+pdu[128] + String-Heap + Call-Chain ≈ 4KB knapp.
    xTaskCreatePinnedToCore([](void*) {
        char qbuf[64];
        while (!g_shutdown) {
            // Greeting prüfen
            if (send_greeting) {
                send_greeting = false;
                vTaskDelay(pdMS_TO_TICKS(150));
                elm_send("\r\nELM327 v1.5\r\n>");
            }
            // Befehle aus Queue abarbeiten
            while (xQueueReceive(s_cmd_queue, qbuf, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (g_shutdown) break;
                process_command(qbuf);
            }
        }
        syslog("ELM", "Worker beendet (Shutdown)");
        vTaskDelete(NULL);
    }, "elm_worker", 6144, nullptr, 2, nullptr, 1);
    syslog("ELM", "Kompatibel: ABRP, Torque Pro, OBD Fusion, Car Scanner");
}

bool elm327_connected() { return ble_connected; }