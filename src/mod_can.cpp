#include "mod_can.h"
#include "mod_sleep.h"
#include "mod_logs.h"
#include "driver/twai.h"
#include "mod_pmu.h"
#include "mod_gyro.h"
#include "config.h"

// CAN TX nur bei VBUS (Auto an) + Gyro-Bewegung innerhalb GYRO_HOLD_MS
static bool can_tx_allowed() {
    if (!pmu_is_vbus_in()) return false;
    uint32_t last = gyro_last_shake_ms();
    if (last == 0) return false;
    return (millis() - last) < GYRO_HOLD_MS;
}

// ============================================================
//  mod_can - CAN Bus, UDS, OBD2, Scanner
// ============================================================

// ============================================================
//  VAG UDS Steuergeräte (aus ODIS extrahiert)
// ============================================================
const UDSModule VAG_MODULES[] = {
    { "Motor/Drive ECU 1",          0x7E0, 0x7E8 },
    { "Motor/Drive ECU 2",          0x7E2, 0x7EA },
    { "Getriebe/Trans",             0x7E1, 0x7E9 },
    { "Drive Motor Control",        0x7E6, 0x7EE },
    { "Battery Energy Mgmt (BMS)",  0x7E5, 0x7ED },
    { "Battery Charger",            0x765, 0x7CF },
    { "Battery Regulation",         0x728, 0x792 },
    { "Bremse/ABS",                 0x713, 0x77D },
    { "Lenkung/EPS",                0x712, 0x77C },
    { "Airbag",                     0x715, 0x77F },
    { "Gateway",                    0x710, 0x77A },
    { "Kombiinstrument/Tacho",      0x714, 0x77E },
    { "Zentralelektrik",            0x70E, 0x778 },
    { "Komfortsystem",              0x70D, 0x777 },
    { "Klimaanlage/AC",             0x746, 0x7B0 },
    { "ACC/Abstandsregelung",       0x757, 0x7C1 },
    { "ACC 2",                      0x756, 0x7C0 },
    { "Lenkwinkelsensor",           0x751, 0x7BB },
    { "Feststellbremse/EPB",        0x752, 0x7BC },
    { "Reifendruck/TPMS",           0x70B, 0x775 },
    { "Fahrerassistenz Front",      0x74F, 0x7B9 },
    { "Kamera Rückfahrt",           0x769, 0x7D3 },
    { "Kamera System",              0x726, 0x790 },
    { "Spurhalteassistent",         0x74E, 0x7B8 },
    { "Einparkhilfe/PDC",           0x70A, 0x774 },
    { "Sitz Fahrer",                0x74C, 0x7B6 },
    { "Sitz Beifahrer",             0x74D, 0x7B7 },
    { "Tür Fahrertür",              0x74A, 0x7B4 },
    { "Tür Beifahrertür",           0x74B, 0x7B5 },
    { "Tür HR",                     0x73E, 0x7A8 },
    { "Tür HL",                     0x73F, 0x7A9 },
    { "Navigation",                 0x76C, 0x7D6 },
    { "Radio/Infotainment",         0x718, 0x782 },
    { "Head-Up Display",            0x71B, 0x785 },
    { "Telematik",                  0x767, 0x7D1 },
    { "Telefon",                    0x76B, 0x7D5 },
    { "Sound System",               0x76F, 0x7D9 },
    { "Wegfahrsperre",              0x711, 0x77B },
    { "Schlosselektronik",          0x71E, 0x788 },
    { "Scheinwerferregulierung",    0x754, 0x7BE },
    { "Fernlichtassistent",         0x730, 0x79A },
    { "Dachelektronik",             0x72D, 0x797 },
    { "AC Kompressor",              0x719, 0x783 },
    { "Standheizung",               0x76A, 0x7D4 },
    { "Anhängerfunktion",           0x747, 0x7B1 },
    { "Mikrocontroller",            0x763, 0x7CD },
};
const int VAG_MODULE_COUNT = sizeof(VAG_MODULES) / sizeof(VAG_MODULES[0]);

// ============================================================
//  VW MEB bekannte DIDs
// ============================================================
const KnownDID VW_MEB_DIDS[] = {
    { 0xF190, "VIN",              "" },
    { 0xF18C, "ECU Serial",       "" },
    { 0xF187, "Teilenummer",      "" },
    { 0xF189, "SW Version",       "" },
    { 0xF191, "HW Nummer",        "" },
    { 0x0280, "SoC (Ladestand)",  "%" },
    { 0x028C, "SoC real",         "%" },
    { 0x01E4, "HV Spannung",      "V" },
    { 0x01E5, "HV Strom",         "A" },
    { 0x01E3, "Leistung",         "kW" },
    { 0x01A4, "Zelltemp max",     "°C" },
    { 0x01A5, "Zelltemp min",     "°C" },
    { 0x0100, "Geschwindigkeit",  "km/h" },
    { 0x0101, "E-Motor RPM",      "rpm" },
    { 0x0102, "Drehmoment",       "Nm" },
    { 0x0168, "Kühlmittel",       "°C" },
    { 0x0295, "Reichweite",       "km" },
    { 0x1000, "Odometer",         "km" },
};
const int VW_MEB_DID_COUNT = sizeof(VW_MEB_DIDS) / sizeof(VW_MEB_DIDS[0]);

bool can_running  = false;
bool monitor_mode = false;

volatile bool scan_running = false;
volatile bool scan_abort   = false;
uint16_t      scan_step    = 0;
uint16_t      scan_total   = 0;

static ScanParams scan_params;

// ── CAN-Mutex: schützt tx+rx-Transaktionen gegen Überlappung ──
static SemaphoreHandle_t s_can_mutex = NULL;

bool can_lock(uint32_t timeout_ms) {
    if (!s_can_mutex) return false;
    return xSemaphoreTake(s_can_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void can_unlock() {
    if (s_can_mutex) xSemaphoreGive(s_can_mutex);
}

// ============================================================
//  CAN Init / Stop
// ============================================================
bool can_init(uint32_t kbps) {
    if (!s_can_mutex) s_can_mutex = xSemaphoreCreateMutex();

#if CAN_RS_PIN != -1
    pinMode(CAN_RS_PIN, OUTPUT);
    digitalWrite(CAN_RS_PIN, LOW);
#endif

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    g.rx_queue_len = 64;
    g.tx_queue_len = 16;
    g.intr_flags   = ESP_INTR_FLAG_LEVEL3;  // L1-Slots auf S3 oft erschöpft → L3

    twai_timing_config_t t;
    switch (kbps) {
        case 125:  t = TWAI_TIMING_CONFIG_125KBITS(); break;
        case 250:  t = TWAI_TIMING_CONFIG_250KBITS(); break;
        case 1000: t = TWAI_TIMING_CONFIG_1MBITS();   break;
        default:   t = TWAI_TIMING_CONFIG_500KBITS(); break;
    }
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        syslog("CAN", "FEHLER: Treiber Install — Hardware/Lötstelle prüfen");
        return false;
    }
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        syslog("CAN", "FEHLER: TWAI Start — Transceiver prüfen");
        return false;
    }

    can_running = true;
    { char _m[48]; snprintf(_m, sizeof(_m), "SN65HVD230 OK · %ukbps · TX=%d RX=%d", kbps, CAN_TX_PIN, CAN_RX_PIN); syslog("CAN", _m); }
    return true;
}

void can_stop() {
    if (!can_running) return;
    twai_stop();
    twai_driver_uninstall();
    can_running = false;
    syslog("CAN", "Gestoppt");
}

// Prüft echten TWAI-Hardware-Zustand — false bei BUS_OFF
// (BUS_OFF tritt auf wenn Transceiver fehlt und TX-Versuche gescheitert sind)
bool can_hw_ok() {
    if (!can_running) return false;
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return false;
    return st.state == TWAI_STATE_RUNNING;
}

// ============================================================
//  ISO-TP Query (ISO 15765-2)
//  Sendet Single Frame Request, empfängt SF oder FF+CF Response
//  resp enthält reassembled PDU inkl. UDS SID (0x62 ...)
//  Rückgabe: Byte-Anzahl im resp-Buffer oder -1 bei Fehler/Timeout
// ============================================================
// Innere Funktion ohne Mutex — wird von can_isotp_query() unter Lock aufgerufen
static int isotp_query_inner(uint32_t tx_id, uint32_t rx_id,
                             const uint8_t* req, uint8_t req_len,
                             uint8_t* resp, uint16_t max_resp,
                             uint32_t timeout_ms) {
    // Single Frame Request: [PCI, req..., 0x55 padding]
    uint8_t frame[8];
    frame[0] = req_len & 0x0F;  // PCI: SF type=0, length=req_len
    for (uint8_t i = 0; i < req_len; i++) frame[i + 1] = req[i];
    for (uint8_t i = req_len + 1; i < 8; i++) frame[i] = 0x55;

    twai_message_t tx_msg = {};
    tx_msg.identifier       = tx_id;
    tx_msg.data_length_code = 8;
    tx_msg.extd             = (tx_id > 0x7FF) ? 1 : 0;
    memcpy(tx_msg.data, frame, 8);
    if (twai_transmit(&tx_msg, pdMS_TO_TICKS(80)) != ESP_OK) return -1;

    uint16_t total_len  = 0;
    uint16_t received   = 0;
    uint8_t  cf_sn      = 1;
    bool     multiframe = false;
    uint32_t deadline   = millis() + timeout_ms;

    while (millis() < deadline) {
        uint32_t wait_ms = deadline - millis();
        twai_message_t rx = {};
        if (twai_receive(&rx, pdMS_TO_TICKS(wait_ms > 50 ? 50 : wait_ms)) != ESP_OK) continue;
        if (rx_id != 0 && rx.identifier != rx_id) continue;
        if (rx.data_length_code < 2) continue;

        uint8_t pci_type = rx.data[0] >> 4;

        if (pci_type == 0x0) {
            // Single Frame Response
            uint8_t sf_len = rx.data[0] & 0x0F;
            if (sf_len == 0 || sf_len > 7) continue;
            uint16_t copy = sf_len < max_resp ? sf_len : max_resp;
            memcpy(resp, rx.data + 1, copy);
            return (int)copy;

        } else if (pci_type == 0x1) {
            // First Frame: [0x1H 0xLL D0..D5]
            total_len = ((uint16_t)(rx.data[0] & 0x0F) << 8) | rx.data[1];
            uint16_t copy = (total_len < 6 ? total_len : 6);
            if (copy > max_resp) copy = max_resp;
            memcpy(resp, rx.data + 2, copy);
            received   = copy;
            multiframe = true;

            // Flow Control: ContinueToSend, BlockSize=0, STmin=0
            uint8_t fc[8] = {0x30, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55};
            twai_message_t fc_msg = {};
            fc_msg.identifier       = tx_id;
            fc_msg.data_length_code = 8;
            fc_msg.extd             = (tx_id > 0x7FF) ? 1 : 0;
            memcpy(fc_msg.data, fc, 8);
            twai_transmit(&fc_msg, pdMS_TO_TICKS(80));
            cf_sn = 1;

            if (received >= total_len || received >= max_resp) return (int)received;

        } else if (pci_type == 0x2 && multiframe) {
            // Consecutive Frame: [0x2N D0..D6]
            uint8_t sn = rx.data[0] & 0x0F;
            if (sn != cf_sn) return (int)received;  // Sequenzfehler
            cf_sn = (cf_sn + 1) & 0x0F;

            uint16_t remain = total_len - received;
            uint16_t copy   = remain < 7 ? remain : 7;
            if (received + copy > max_resp) copy = max_resp - received;
            memcpy(resp + received, rx.data + 1, copy);
            received += copy;

            if (received >= total_len || received >= max_resp) return (int)received;
        }
    }

    return (multiframe && received > 0) ? (int)received : -1;
}

int can_isotp_query(uint32_t tx_id, uint32_t rx_id,
                    const uint8_t* req, uint8_t req_len,
                    uint8_t* resp, uint16_t max_resp,
                    uint32_t timeout_ms) {
    if (!can_running || req_len == 0 || req_len > 7) return -1;
    if (!can_tx_allowed()) return -1;
    if (!can_lock(timeout_ms + 200)) return -1;
    int result = isotp_query_inner(tx_id, rx_id, req, req_len, resp, max_resp, timeout_ms);
    can_unlock();
    return result;
}

// ============================================================
//  Senden
// ============================================================
bool can_tx(uint32_t id, uint8_t* data, uint8_t len, const char* label) {
    if (!can_running) return false;
    // BLE Wächter prüfen
    if (!can_tx_allowed()) {
        syslog("CAN", "TX gesperrt — kein VBUS oder keine Bewegung");
        return false;
    }
    twai_message_t msg = {};
    msg.identifier       = id;
    msg.data_length_code = len;
    msg.extd             = (id > 0x7FF) ? 1 : 0;
    memcpy(msg.data, data, len);
    if (twai_transmit(&msg, pdMS_TO_TICKS(80)) != ESP_OK) return false;
    log_add(true, id, data, len, label);
    return true;
}

// ============================================================
//  Empfangen
// ============================================================
int can_rx_collect(uint32_t resp_id, uint32_t timeout_ms,
                   uint8_t out_frames[][8], uint8_t out_lens[],
                   uint32_t out_ids[], int max_frames) {
    int cnt = 0;
    uint32_t deadline = millis() + timeout_ms;

    while (millis() < deadline && cnt < max_frames) {
        twai_message_t rx = {};
        if (twai_receive(&rx, pdMS_TO_TICKS(15)) == ESP_OK) {
            if (resp_id == 0 || rx.identifier == resp_id) {
                if (out_frames) memcpy(out_frames[cnt], rx.data, rx.data_length_code);
                if (out_lens)   out_lens[cnt] = rx.data_length_code;
                if (out_ids)    out_ids[cnt]  = rx.identifier;
                cnt++;

                // Info dekodieren
                char info[LOG_INFO_LEN] = "";
                if (rx.data_length_code >= 4 && rx.data[1] == 0x62) {
                    snprintf(info, LOG_INFO_LEN, "UDS OK DID=0x%02X%02X", rx.data[2], rx.data[3]);
                } else if (rx.data_length_code >= 2 && rx.data[1] == 0x7F) {
                    uint8_t nrc = rx.data_length_code >= 4 ? rx.data[3] : 0;
                    snprintf(info, LOG_INFO_LEN, "UDS NRC=0x%02X", nrc);
                } else if (rx.data_length_code >= 3 && rx.data[1] == 0x41) {
                    snprintf(info, LOG_INFO_LEN, "OBD2 PID=0x%02X", rx.data[2]);
                }
                log_add(false, rx.identifier, rx.data, rx.data_length_code, info);
            }
        }
    }
    return cnt;
}

// ============================================================
//  UDS Service 0x22
// ============================================================
bool uds_read_did(uint32_t req_id, uint32_t resp_id, uint16_t did,
                  uint8_t* resp_data, uint8_t* resp_len) {
    if (!can_running) return false;
    if (!can_tx_allowed()) return false;
    if (!can_lock()) return false;

    // UDS Request: Service 0x22 + DID (2 Bytes) = 3 Bytes PDU
    uint8_t req[3] = { 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
    uint8_t buf[64];

    int n = isotp_query_inner(req_id, resp_id, req, 3, buf, sizeof(buf), UDS_TIMEOUT_MS);
    can_unlock();

    // Antwort prüfen: buf[0]=0x62, buf[1..2]=DID, buf[3..]=Daten
    if (n < 3 || buf[0] != 0x62) return false;

    uint16_t rdid = ((uint16_t)buf[1] << 8) | buf[2];
    if (rdid != did) return false;

    if (resp_data && resp_len) {
        *resp_len = (uint8_t)(n - 3);
        memcpy(resp_data, buf + 3, *resp_len);
    }
    return true;
}

void uds_tester_present(uint32_t req_id) {
    uint8_t tp[8] = { 0x02, 0x3E, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    can_tx(req_id, tp, 8, "TesterPresent");
    vTaskDelay(pdMS_TO_TICKS(50));
}

// ============================================================
//  Scanner Implementierungen
// ============================================================
static void scan_modules() {
    scan_total = VAG_MODULE_COUNT;
    ws_scan_status("Scanne VAG Steuergeräte...", 0, scan_total);

    for (int i = 0; i < VAG_MODULE_COUNT && !scan_abort; i++) {
        const UDSModule& m = VAG_MODULES[i];
        scan_step = i + 1;

        char status[64];
        snprintf(status, 64, "Teste %s (0x%03X)...", m.name, m.req_id);
        ws_scan_status(status, scan_step, scan_total);

        uint8_t req[8] = { 0x03, 0x22, 0xF1, 0x90, 0xCC, 0xCC, 0xCC, 0xCC };
        char label[LOG_INFO_LEN];
        snprintf(label, LOG_INFO_LEN, "→ %s", m.name);

        int n = 0;
        if (can_lock()) {
            can_tx(m.req_id, req, 8, label);
            uds_tester_present(m.req_id);
            uint8_t frames[4][8]; uint8_t lens[4]; uint32_t ids[4];
            n = can_rx_collect(m.resp_id, UDS_TIMEOUT_MS, frames, lens, ids, 4);
            can_unlock();
        }

        if (n > 0) {
            snprintf(status, 64, "✓ AKTIV: %s", m.name);
            ws_scan_status(status, scan_step, scan_total);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_DELAY_MS));
    }
}

static void scan_dids(uint32_t req_id, uint32_t resp_id, const char* module_name) {
    scan_total = VW_MEB_DID_COUNT;
    char status[64];
    snprintf(status, 64, "Lese DIDs von %s...", module_name);
    ws_scan_status(status, 0, scan_total);
    uds_tester_present(req_id);

    for (int i = 0; i < VW_MEB_DID_COUNT && !scan_abort; i++) {
        const KnownDID& d = VW_MEB_DIDS[i];
        scan_step = i + 1;
        snprintf(status, 64, "DID 0x%04X: %s", d.did, d.name);
        ws_scan_status(status, scan_step, scan_total);

        uint8_t resp[8]; uint8_t rlen = 0;
        bool ok = uds_read_did(req_id, resp_id, d.did, resp, &rlen);
        if (ok) {
            snprintf(status, 64, "✓ %s OK", d.name);
            ws_scan_status(status, scan_step, scan_total);
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_DELAY_MS));
    }
}

static void scan_obd_pids() {
    scan_total = 256;
    ws_scan_status("OBD2 Brute-Force PID 0x00-0xFF...", 0, scan_total);

    for (int pid = 0; pid < 256 && !scan_abort; pid++) {
        scan_step = pid + 1;
        uint8_t req[8] = { 0x02, 0x01, (uint8_t)pid, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
        char label[LOG_INFO_LEN];
        snprintf(label, LOG_INFO_LEN, "OBD2 Mode01 PID=0x%02X", pid);

        if (can_lock()) {
            can_tx(0x7DF, req, 8, label);
            uint8_t frames[4][8]; uint8_t lens[4]; uint32_t ids[4];
            can_rx_collect(0, OBD_TIMEOUT_MS, frames, lens, ids, 4);
            can_unlock();
        }

        if (pid % 16 == 0) {
            char s[64]; snprintf(s, 64, "OBD2 Scan: PID 0x%02X...", pid);
            ws_scan_status(s, scan_step, scan_total);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ============================================================
//  Tasks
// ============================================================
void scan_task(void* pv) {
    ScanParams* p = (ScanParams*)pv;
    scan_abort = false;

    switch (p->mode) {
        case 1: scan_modules();                              break;
        case 2: scan_dids(p->req_id, p->resp_id, p->name); break;
        case 3: scan_obd_pids();                            break;
    }

    scan_running = false;
    ws_scan_status("Scan abgeschlossen!", scan_step, scan_total);
    Serial.println("[SCAN] Fertig");
    vTaskDelete(NULL);
}

void monitor_task(void*) {
    uint32_t last_hw_check = 0;

    while (!g_shutdown) {
        // Alle 5s echten TWAI-Zustand prüfen → BUS_OFF = Transceiver fehlt/defekt
        if (millis() - last_hw_check >= 5000) {
            last_hw_check = millis();
            if (can_running) {
                twai_status_info_t st;
                if (twai_get_status_info(&st) == ESP_OK &&
                    st.state == TWAI_STATE_BUS_OFF) {
                    Serial.println("[CAN] BUS_OFF — Recovery...");
                    syslog("CAN", "BUS_OFF erkannt — Recovery läuft");
                    twai_initiate_recovery();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (twai_get_status_info(&st) == ESP_OK &&
                        st.state == TWAI_STATE_RUNNING) {
                        syslog("CAN", "BUS_OFF Recovery OK");
                    } else {
                        can_running = false;
                        syslog("CAN", "FEHLER: BUS_OFF Recovery fehlgeschlagen — Transceiver prüfen");
                    }
                }
            }
        }

        if (monitor_mode && can_running && !scan_running) {
            // tryLock: überspringen wenn eine Transaktion läuft
            if (can_lock(30)) {
                twai_message_t rx = {};
                if (twai_receive(&rx, pdMS_TO_TICKS(30)) == ESP_OK) {
                    char info[LOG_INFO_LEN] = "";
                    if (rx.data_length_code >= 4 && rx.data[1] == 0x62) {
                        snprintf(info, LOG_INFO_LEN, "UDS OK DID=0x%02X%02X", rx.data[2], rx.data[3]);
                    } else if (rx.data_length_code >= 2 && rx.data[1] == 0x7F) {
                        snprintf(info, LOG_INFO_LEN, "UDS NRC=0x%02X",
                                 rx.data_length_code >= 4 ? rx.data[3] : 0);
                    } else if (rx.data_length_code >= 3 && rx.data[1] == 0x41) {
                        snprintf(info, LOG_INFO_LEN, "OBD2 PID=0x%02X", rx.data[2]);
                    }
                    log_add(false, rx.identifier, rx.data, rx.data_length_code, info);
                }
                can_unlock();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    syslog("CAN", "Monitor beendet (Shutdown)");
    vTaskDelete(NULL);
}

// Scan von außen starten (wird vom Webserver aufgerufen)
void can_start_scan(int mode, uint32_t req_id, uint32_t resp_id, const char* name) {
    if (scan_running) return;
    scan_params.mode    = mode;
    scan_params.req_id  = req_id;
    scan_params.resp_id = resp_id;
    strncpy(scan_params.name, name, 39);
    scan_running = true;
    scan_step    = 0;
    scan_total   = 0;
    xTaskCreatePinnedToCore(scan_task, "Scan", 6144, &scan_params, 1, NULL, 0);
}

// ============================================================
//  ELM327 Varianten - loggen nach /elm.log
// ============================================================
bool can_tx_elm(uint32_t id, uint8_t* data, uint8_t len, const char* label) {
    if (!can_running) return false;
    if (!can_tx_allowed()) return false;
    twai_message_t msg = {};
    msg.identifier       = id;
    msg.data_length_code = len;
    msg.extd             = (id > 0x7FF) ? 1 : 0;
    memcpy(msg.data, data, len);
    if (twai_transmit(&msg, pdMS_TO_TICKS(80)) != ESP_OK) return false;
    log_add(true, id, data, len, label, LOG_SRC_ELM);
    return true;
}

int can_rx_collect_elm(uint32_t resp_id, uint32_t timeout_ms,
                       uint8_t out_frames[][8], uint8_t out_lens[],
                       uint32_t out_ids[], int max_frames) {
    int cnt = 0;
    uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline && cnt < max_frames) {
        twai_message_t rx = {};
        if (twai_receive(&rx, pdMS_TO_TICKS(15)) == ESP_OK) {
            if (resp_id == 0 || rx.identifier == resp_id) {
                if (out_frames) memcpy(out_frames[cnt], rx.data, rx.data_length_code);
                if (out_lens)   out_lens[cnt] = rx.data_length_code;
                if (out_ids)    out_ids[cnt]  = rx.identifier;
                cnt++;
                char info[LOG_INFO_LEN] = "";
                if (rx.data_length_code >= 4 && rx.data[1] == 0x62)
                    snprintf(info, LOG_INFO_LEN, "UDS OK DID=0x%02X%02X", rx.data[2], rx.data[3]);
                else if (rx.data_length_code >= 2 && rx.data[1] == 0x7F)
                    snprintf(info, LOG_INFO_LEN, "UDS NRC=0x%02X",
                             rx.data_length_code >= 4 ? rx.data[3] : 0);
                else if (rx.data_length_code >= 3 && rx.data[1] == 0x41)
                    snprintf(info, LOG_INFO_LEN, "OBD2 PID=0x%02X", rx.data[2]);
                log_add(false, rx.identifier, rx.data, rx.data_length_code,
                        info, LOG_SRC_ELM);
            }
        }
    }
    return cnt;
}

int can_isotp_query_elm(uint32_t tx_id, uint32_t rx_id,
                        uint32_t fc_id, const uint8_t* fc_data, uint8_t fc_len,
                        const uint8_t* req, uint8_t req_len,
                        uint8_t out_frames[][8], uint8_t out_lens[],
                        uint32_t out_ids[], int max_frames,
                        uint32_t timeout_ms) {
    if (!can_running || req_len == 0 || req_len > 7 || max_frames < 1) return 0;
    if (!can_tx_allowed()) return 0;

    // FC-ID: 0 = tx_id verwenden (Standard-Verhalten)
    uint32_t fc_tx_id = (fc_id != 0) ? fc_id : tx_id;

    // RX-Queue leeren: Hintergrund-CAN-Traffic entfernen bevor Request gesendet wird
    // (verhindert Queue-Überlauf der die ECU-Antwort verdrängen würde)
    {
        twai_message_t drain;
        int flushed = 0;
        while (twai_receive(&drain, 0) == ESP_OK && flushed < 128) flushed++;
        if (flushed > 10) Serial.printf("[CAN] ELM: %d Frames geflusht\n", flushed);
    }

    // Single Frame Request senden
    uint8_t frame[8];
    frame[0] = req_len & 0x0F;
    for (uint8_t i = 0; i < req_len; i++) frame[i + 1] = req[i];
    for (uint8_t i = req_len + 1; i < 8; i++) frame[i] = 0x55;

    twai_message_t tx_msg = {};
    tx_msg.identifier       = tx_id;
    tx_msg.data_length_code = 8;
    tx_msg.extd             = (tx_id > 0x7FF) ? 1 : 0;
    memcpy(tx_msg.data, frame, 8);
    if (twai_transmit(&tx_msg, pdMS_TO_TICKS(80)) != ESP_OK) return 0;

    // Log TX
    log_add(true, tx_id, frame, 8,
            (req_len >= 3 && req[0] == 0x22) ?
            (char*)"ELM UDS TX" : (char*)"ELM TX", LOG_SRC_ELM);

    int frame_count = 0;
    bool multiframe = false;
    uint16_t total_len = 0;
    uint16_t received = 0;
    uint8_t cf_sn = 1;
    uint32_t deadline = millis() + timeout_ms;

    while (millis() < deadline && frame_count < max_frames) {
        uint32_t wait_ms = deadline - millis();
        twai_message_t rx = {};
        if (twai_receive(&rx, pdMS_TO_TICKS(wait_ms > 50 ? 50 : wait_ms)) != ESP_OK) continue;
        if (rx_id != 0 && rx.identifier != rx_id) continue;
        if (rx.data_length_code < 2) continue;

        uint8_t pci_type = rx.data[0] >> 4;

        if (pci_type == 0x0) {
            // Single Frame → direkt zurückgeben
            memcpy(out_frames[frame_count], rx.data, rx.data_length_code);
            out_lens[frame_count] = rx.data_length_code;
            out_ids[frame_count]  = rx.identifier;
            frame_count++;
            break;

        } else if (pci_type == 0x1) {
            // First Frame → speichern + Flow Control senden
            total_len = ((uint16_t)(rx.data[0] & 0x0F) << 8) | rx.data[1];
            memcpy(out_frames[frame_count], rx.data, rx.data_length_code);
            out_lens[frame_count] = rx.data_length_code;
            out_ids[frame_count]  = rx.identifier;
            frame_count++;
            received = 6;
            multiframe = true;

            // Flow Control Frame senden (mit konfigurierter FC-ID und Daten)
            uint8_t fc_frame[8] = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            if (fc_data && fc_len > 0) memcpy(fc_frame, fc_data, fc_len < 8 ? fc_len : 8);

            twai_message_t fc_msg = {};
            fc_msg.identifier       = fc_tx_id;
            fc_msg.data_length_code = 8;
            fc_msg.extd             = (fc_tx_id > 0x7FF) ? 1 : 0;
            memcpy(fc_msg.data, fc_frame, 8);
            twai_transmit(&fc_msg, pdMS_TO_TICKS(80));

            cf_sn = 1;

        } else if (pci_type == 0x2 && multiframe) {
            // Consecutive Frame
            uint8_t sn = rx.data[0] & 0x0F;
            if (sn != cf_sn) break;  // Sequenzfehler
            cf_sn = (cf_sn + 1) & 0x0F;

            memcpy(out_frames[frame_count], rx.data, rx.data_length_code);
            out_lens[frame_count] = rx.data_length_code;
            out_ids[frame_count]  = rx.identifier;
            frame_count++;
            received += 7;

            if (received >= total_len) break;  // Komplett
        }
    }

    return frame_count;
}

void can_sniff(uint32_t duration_ms) {
    if (!can_hw_ok()) {
        Serial.println("[CAN] Sniff: Bus nicht aktiv");
        return;
    }
    Serial.printf("┌─── CAN Sniff (%lu ms) ──────────────────┐\n", duration_ms);
    Serial.println("│ ID          DLC  Daten");

    uint32_t start = millis();
    int count = 0;
    while ((millis() - start) < duration_ms) {
        twai_message_t rx = {};
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK) {
            if (rx.extd)
                Serial.printf("│ X %08X  %d    ", rx.identifier, rx.data_length_code);
            else
                Serial.printf("│   %03X       %d    ", rx.identifier, rx.data_length_code);
            for (int i = 0; i < rx.data_length_code; i++)
                Serial.printf("%02X ", rx.data[i]);
            Serial.println();
            count++;
        }
    }
    Serial.printf("└─── %d Frames empfangen ──────────────────┘\n", count);
}