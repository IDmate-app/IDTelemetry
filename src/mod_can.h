#pragma once
#include "shared.h"

// ============================================================
//  mod_can - CAN Bus, UDS, OBD2, Scanner
// ============================================================

// Init / Stop
bool can_init(uint32_t kbps = CAN_SPEED_KBPS);
void can_stop();

// CAN-Bus Mutex — schützt gegen gleichzeitige Transaktionen aus
// mehreren Tasks (Telemetrie, ELM327, Web-Scanner, Monitor).
// can_isotp_query() und uds_read_did() locken intern.
// Für manuelle tx+rx-Paare: can_lock() VOR tx, can_unlock() NACH rx.
bool can_lock(uint32_t timeout_ms = 500);
void can_unlock();

// Echter Hardware-Check: TWAI-State (false bei BUS_OFF oder nicht gestartet)
bool can_hw_ok();

// ISO-TP Query (ISO 15765-2): sendet Single Frame, empfängt SF/FF+CF
// resp = reassembled PDU inkl. UDS SID (0x62...), returns Byte-Anzahl oder -1
int can_isotp_query(uint32_t tx_id, uint32_t rx_id,
                    const uint8_t* req, uint8_t req_len,
                    uint8_t* resp, uint16_t max_resp,
                    uint32_t timeout_ms);

// Senden
bool can_tx(uint32_t id, uint8_t* data, uint8_t len, const char* label);

// Empfangen (sammelt bis timeout, resp_id=0 = alle)
int can_rx_collect(uint32_t resp_id, uint32_t timeout_ms,
                   uint8_t out_frames[][8], uint8_t out_lens[],
                   uint32_t out_ids[], int max_frames);

// UDS Service 0x22 Read Data By Identifier
bool uds_read_did(uint32_t req_id, uint32_t resp_id, uint16_t did,
                  uint8_t* resp_data = nullptr, uint8_t* resp_len = nullptr);

// Tester Present (hält Session offen)
void uds_tester_present(uint32_t req_id);

// Monitor Task (auf Core 0)
void monitor_task(void* pv);

// Scan Task
void scan_task(void* pv);

// Scan von außen starten (vom Webserver aufgerufen)
void can_start_scan(int mode, uint32_t req_id, uint32_t resp_id, const char* name);

// ELM327 Varianten — loggen nach /elm.log statt /scan.log
bool can_tx_elm(uint32_t id, uint8_t* data, uint8_t len, const char* label);
int  can_rx_collect_elm(uint32_t resp_id, uint32_t timeout_ms,
                        uint8_t out_frames[][8], uint8_t out_lens[],
                        uint32_t out_ids[], int max_frames);

// ISO-TP Query mit separatem Flow Control Header (für VW Extended CAN via ELM327)
// fc_id = CAN-ID für Flow Control Frame (0 = tx_id verwenden)
// fc_data/fc_len = Flow Control Payload (default: 30 00 00)
// Gibt rohe CAN-Frames zurück (nicht reassembled), für ELM327-Formatierung
int can_isotp_query_elm(uint32_t tx_id, uint32_t rx_id,
                        uint32_t fc_id, const uint8_t* fc_data, uint8_t fc_len,
                        const uint8_t* req, uint8_t req_len,
                        uint8_t out_frames[][8], uint8_t out_lens[],
                        uint32_t out_ids[], int max_frames,
                        uint32_t timeout_ms);

// CAN Sniffer: alle Frames für duration_ms auf Serial dumpen
void can_sniff(uint32_t duration_ms);