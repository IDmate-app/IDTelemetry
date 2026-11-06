#include "mod_telem_store.h"
#include <Preferences.h>
#include "mod_logs.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

// ============================================================
//  Layout
//    Partition:    5 MB, Subtype 0x40, Label "telem"
//    Erase-Block:  4 KB (Flash-Sector)
//    Slot:         128 B  (32 Slots pro Block)
//    Total Slots:  5*1024*1024 / 128 = 40960
//
//  Status-Byte (1->0 Bit-Flips ohne Erase moeglich):
//    0xFF = leer (Default nach Erase, alles 1en)
//    0xFE = geschrieben, ungesendet
//    0xFC = gesendet (ack'd)
//
//  Append-Pfad (atomar):
//    1. Inhalt in leeren Slot schreiben (Status bleibt 0xFF)
//    2. Status auf 0xFE setzen  ← einziger Commit-Schritt
//    Crash vor Schritt 2 → Slot bleibt 0xFF, halb-geschriebener
//    Inhalt wird beim naechsten Write einfach ueberschrieben.
//
//  Ack-Pfad:
//    Status von 0xFE auf 0xFC setzen — 1 Bit-Flip, atomar.
//    Crash dazwischen → Slot bleibt 0xFE, Re-Sending → Duplikat
//    moeglich aber kein Datenverlust.
//
//  Block-Recycling:
//    Wenn der naechste zu schreibende Slot in einem Block liegt
//    der nicht komplett 0xFF ist, wird der Block erased.
//    Falls dieser Block ungesendete 0xFE-Slots enthielt → Datenverlust
//    (Buffer voll, aelteste Zeilen werden eingeholt). Wird geloggt.
// ============================================================

#define SLOT_SIZE          128u
#define SECTOR_SIZE        4096u
#define SLOTS_PER_SECTOR   (SECTOR_SIZE / SLOT_SIZE)   // 32

#define STATUS_EMPTY       0xFF
#define STATUS_WRITTEN     0xFE
#define STATUS_SENT        0xFC

// Auf-Disk Layout fuer eine Zeile (ab 1.5.0): Gueltig-Flags als Bitmaske,
// damit 29 Felder in 128 B passen (vorher 1 Byte pro Feld: max. 24).
struct __attribute__((packed)) Slot {
    uint8_t  status;                          // 1
    uint8_t  reserved[3];                     // 3   → 4   (reserved[0] = Layout-Version, FIXES B.5)
    uint32_t unix_s;                          // 4   → 8
    float    values[TELEM_FIELD_COUNT];       // 4·N
    uint32_t valid_mask;                      // 4   Bit i = Feld i gueltig
    uint8_t  pad[SLOT_SIZE - 12 - 4 * TELEM_FIELD_COUNT];
};
static_assert(sizeof(Slot) == SLOT_SIZE, "Slot size must be 128");
static_assert(TELEM_FIELD_COUNT <= 29, "mehr als 29 Felder passen nicht in 128 B");

// Layout bis 1.4.x: 22 Felder, 1 Byte pro Gueltig-Flag. Wird beim Lesen noch
// verstanden, damit ein Update den ungesendeten Backlog nicht wegwirft.
#define LEGACY22_FIELDS 22
struct __attribute__((packed)) SlotLegacy22 {
    uint8_t  status;
    uint8_t  reserved[3];
    uint32_t unix_s;
    float    values[LEGACY22_FIELDS];         // 88  → 96
    uint8_t  valid[LEGACY22_FIELDS];          // 22  → 118
    uint8_t  pad[SLOT_SIZE - 118];
};
static_assert(sizeof(SlotLegacy22) == SLOT_SIZE, "Legacy slot size must be 128");
static_assert(TELEM_FIELD_COUNT >= LEGACY22_FIELDS, "Felder werden nur angehaengt");

// FIXES B.5: reserved[0] trägt die Layout-Version. Ändert sich das Layout,
// dekodiert der Boot-Scan sonst alte Slots im neuen Layout → verschobene
// Garbage-Werte gehen als plausible Telemetrie raus.
// 0xFF und 22 = Layout bis 1.4.x (unversioniert bzw. 22 Felder, Byte-Flags).
// Ab 1.5.0: 0x80 | Feldanzahl (Bitmasken-Layout). Anderer Wert → verworfen.
#define SLOT_LAYOUT_LEGACY22 ((uint8_t)LEGACY22_FIELDS)
#define SLOT_LAYOUT_VERSION  ((uint8_t)(0x80 | TELEM_FIELD_COUNT))
static_assert(SLOT_LAYOUT_VERSION != 0xFF, "Layout-Version muss != 0xFF sein");

static const esp_partition_t* s_part        = nullptr;
static uint32_t                s_total_slots = 0;
static uint32_t                s_head        = 0;   // naechster Schreibslot
static uint32_t                s_tail        = 0;   // aeltester ungesendeter Slot
static uint32_t                s_pending     = 0;
static SemaphoreHandle_t       s_mtx         = nullptr;

// Hilfe: Slot-Index → Byte-Offset in Partition
static inline uint32_t slot_offset(uint32_t idx) { return idx * SLOT_SIZE; }

// Status-Byte eines Slots lesen
static uint8_t read_status(uint32_t idx) {
    uint8_t s = 0xFF;
    if (esp_partition_read(s_part, slot_offset(idx), &s, 1) != ESP_OK) return 0xFF;
    return s;
}

// Status-Byte ueberschreiben (nur 1→0 Bit-Flips, kein Erase noetig)
static bool write_status(uint32_t idx, uint8_t new_status) {
    return esp_partition_write(s_part, slot_offset(idx), &new_status, 1) == ESP_OK;
}

// Block erasen, der den Slot enthaelt
static bool erase_block_for_slot(uint32_t idx) {
    uint32_t block_start = (idx / SLOTS_PER_SECTOR) * SLOTS_PER_SECTOR;
    return esp_partition_erase_range(s_part, slot_offset(block_start), SECTOR_SIZE) == ESP_OK;
}

// FIXES B.1: Zählt ungesendete (0xFE) Slots in einem Block und korrigiert
// pending/tail, BEVOR der Block erased wird — sonst driftet s_pending und es
// gehen ungesendete Rows ohne Buchung verloren (Header-Doku-Zusatz B.1).
static void account_block_before_erase(uint32_t block_start) {
    block_start = (block_start / SLOTS_PER_SECTOR) * SLOTS_PER_SECTOR;
    for (uint32_t i = 0; i < SLOTS_PER_SECTOR; i++) {
        uint32_t idx = (block_start + i) % s_total_slots;
        if (read_status(idx) == STATUS_WRITTEN) {
            if (s_pending > 0) s_pending--;
            if (idx == s_tail) s_tail = (s_tail + 1) % s_total_slots;  // tail aus dem Block schieben
        }
    }
}

// FIXES B.1: Stellt sicher, dass s_head auf einem erased (0xFF) Slot steht.
// boot_scan() kann s_head MITTEN im Block auf einen 0xFC/0xFE-Altslot setzen
// (Wrap-Backlog, Crash beim Drain, abgebrochener Erase). esp_partition_write
// kann in NOR-Flash nur 1→0 flippen → ein Schreiben in nicht-gelöschte Zellen
// ver-UND-et den Body mit Altdaten und der Status-Commit scheitert still → Row
// korrupt/verloren, s_pending driftet. Darum vor JEDEM Write den Ziel-Slot
// prüfen und ggf. zur nächsten Blockgrenze springen + erasen.
static void ensure_head_writable() {
    if (read_status(s_head) == STATUS_EMPTY) return;   // bereits beschreibbar

    uint32_t block_start = (s_head / SLOTS_PER_SECTOR) * SLOTS_PER_SECTOR;
    uint32_t target;
    if (s_head == block_start) {
        // Head an der Blockgrenze → vor head liegt kein eigener frischer Slot in
        // diesem Block → diesen Block direkt neu starten.
        target = block_start;
    } else {
        // Head mitten im Block → die Slots davor können eigene, frisch
        // geschriebene 0xFE-Rows sein. Diesen Block NICHT erasen, sondern an der
        // nächsten Blockgrenze weiterschreiben.
        target = (block_start + SLOTS_PER_SECTOR) % s_total_slots;
    }
    account_block_before_erase(target);
    erase_block_for_slot(target);
    s_head = target;
}

// Boot-Scan: liest alle Status-Bytes, bestimmt tail/head/pending.
// Algorithmus: zwei Zeiger laufen rund, finde laengste Folge von 0xFE.
// Wenn keine 0xFE existieren → Buffer leer, head=tail=0.
// Wenn alle 0xFE → Buffer voll, tail=head=0.
static void boot_scan() {
    s_pending = 0;

    // Status-Bytes effizient lesen: sektorweise 4 KB lesen, 32 Status extrahieren.
    uint8_t* sec_buf = (uint8_t*)malloc(SECTOR_SIZE);
    if (!sec_buf) {
        syslog("STORE", "FEHLER: malloc fuer Sector-Buffer");
        return;
    }

    uint32_t num_sectors = s_total_slots / SLOTS_PER_SECTOR;
    uint8_t* status = (uint8_t*)malloc(s_total_slots);
    if (!status) {
        free(sec_buf);
        syslog("STORE", "FEHLER: malloc fuer Status-Array");
        return;
    }

    // 1.6.5: neuesten beschriebenen Slot merken (hoechster Zeitstempel).
    // Ohne ungesendete Zeilen setzte der Boot-Scan head auf 0 — jede Fahrt
    // begann vorne und loeschte die gesendeten Zeilen der letzten Fahrten,
    // die "Erneut senden" noch braucht (Log 2026-11-06: "Boot-Scan: leer").
    uint32_t newest_idx = UINT32_MAX, newest_ts = 0;
    for (uint32_t sec = 0; sec < num_sectors; sec++) {
        if (esp_partition_read(s_part, sec * SECTOR_SIZE, sec_buf, SECTOR_SIZE) != ESP_OK) {
            for (uint32_t s = 0; s < SLOTS_PER_SECTOR; s++) status[sec * SLOTS_PER_SECTOR + s] = 0xFF;
            continue;
        }
        for (uint32_t s = 0; s < SLOTS_PER_SECTOR; s++) {
            const uint8_t* sl = sec_buf + s * SLOT_SIZE;
            status[sec * SLOTS_PER_SECTOR + s] = sl[0];
            if (sl[0] == STATUS_WRITTEN || sl[0] == STATUS_SENT) {
                uint32_t ts; memcpy(&ts, sl + 4, 4);
                if (ts >= 1704067200u && ts != 0xFFFFFFFFu && ts >= newest_ts) { newest_ts = ts; newest_idx = sec * SLOTS_PER_SECTOR + s; }
            }
        }
    }
    free(sec_buf);

    // Strategie: finde Tail = erster 0xFE nach einem nicht-0xFE Slot.
    // Falls alle 0xFE → tail=0. Falls keine 0xFE → empty, tail=head=0.
    uint32_t count_written = 0;
    for (uint32_t i = 0; i < s_total_slots; i++) {
        if (status[i] == STATUS_WRITTEN) count_written++;
    }

    if (count_written == 0) {
        // Hinter der neuesten Zeile weiterschreiben, nicht bei 0 (1.6.5).
        s_head = (newest_idx == UINT32_MAX) ? 0 : (newest_idx + 1) % s_total_slots;
        s_tail = s_head;
        s_pending = 0;
        free(status);
        char m[96]; snprintf(m, sizeof(m), "Boot-Scan: nichts ungesendet · weiter bei Slot %u (von %u)",
                             (unsigned)s_head, (unsigned)s_total_slots);
        syslog("STORE", m);
        return;
    }

    // Tail finden: gehe rund herum, suche Stelle wo "nicht-FE → FE" wechselt.
    // Wenn solche Stelle existiert → tail dort. Sonst (alle FE oder vermischt
    // ohne klare Bloecke nach Crash) → nimm Slot 0 als Tail.
    s_tail = 0;
    for (uint32_t i = 0; i < s_total_slots; i++) {
        uint32_t prev_i = (i + s_total_slots - 1) % s_total_slots;
        if (status[i] == STATUS_WRITTEN && status[prev_i] != STATUS_WRITTEN) {
            s_tail = i;
            break;
        }
    }

    // Head finden: ab Tail vorwaerts, erster Slot der nicht 0xFE ist.
    s_head = s_tail;
    for (uint32_t k = 0; k < s_total_slots; k++) {
        uint32_t i = (s_tail + k) % s_total_slots;
        if (status[i] != STATUS_WRITTEN) {
            s_head = i;
            break;
        }
        // Bei k == s_total_slots-1 ohne Treffer → Buffer voll, Head = Tail
        if (k == s_total_slots - 1) s_head = s_tail;
    }

    s_pending = count_written;

    free(status);

    char m[96];
    snprintf(m, sizeof(m), "Boot-Scan: %u Rows pending (tail=%u head=%u capacity=%u)",
             s_pending, s_tail, s_head, s_total_slots);
    syslog("STORE", m);
}

void telem_store_init() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40, "telem");
    if (!s_part) {
        syslog("STORE", "FEHLER: telem-Partition nicht gefunden!");
        return;
    }

    s_total_slots = s_part->size / SLOT_SIZE;

    char m[96];
    snprintf(m, sizeof(m), "Partition gefunden: %u KB · %u Slots à %u B",
             s_part->size / 1024, s_total_slots, SLOT_SIZE);
    syslog("STORE", m);

    // 1.6.0: Partitionstabelle mit zweitem App-Platz — telem liegt an neuer
    // Stelle (0x420000, 3 MB) mitten im alten 5-MB-Bereich. Ohne Loeschen
    // laese boot_scan() Reste des alten Puffers als Zeilen. Lage + Groesse
    // stehen in NVS; aendern sie sich, wird die Partition einmal geloescht.
    {
        Preferences p;
        if (p.begin("telem", false)) {
            uint32_t want = (s_part->address ^ (s_part->size << 4)) | 1u;
            if (p.getUInt("layout", 0) != want) {
                snprintf(m, sizeof(m), "Neues Partitions-Layout · telem @0x%06X wird einmalig geloescht",
                         (unsigned)s_part->address);
                syslog("STORE", m);
                for (uint32_t off = 0; off < s_part->size; off += 0x10000) {
                    uint32_t n = s_part->size - off < 0x10000 ? s_part->size - off : 0x10000;
                    esp_partition_erase_range(s_part, off, n);
                    vTaskDelay(1);
                }
                p.putUInt("layout", want);
                syslog("STORE", "telem geloescht");
            }
            p.end();
        }
    }

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    boot_scan();
    xSemaphoreGive(s_mtx);
}

static bool append_locked(const TelemetryRow& row);

bool telem_store_append(const TelemetryRow& row) {
    if (!s_part || !s_mtx) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool ok = append_locked(row);
    xSemaphoreGive(s_mtx);
    return ok;
}

// Aufrufer haelt s_mtx.
static bool append_locked(const TelemetryRow& row) {

    // Wenn Buffer voll: aeltesten Block opfern (32 Rows weg) damit wir weiter aufnehmen.
    // Das passiert nur nach > 40k Rows ohne Upload (sehr selten).
    if (s_pending >= s_total_slots) {
        uint32_t lost_block_start = (s_tail / SLOTS_PER_SECTOR) * SLOTS_PER_SECTOR;
        erase_block_for_slot(s_tail);
        s_tail = (lost_block_start + SLOTS_PER_SECTOR) % s_total_slots;
        s_pending -= SLOTS_PER_SECTOR;
        syslog("STORE", "Buffer voll · aeltester Block geloescht (32 Rows verloren)");
    }

    // FIXES B.1: Ziel-Slot MUSS gelöscht (0xFF) sein, sonst korrupter Body durch
    // 1→0-only-Flips in NOR-Flash. Deckt auch den Fall ab, dass boot_scan() head
    // mitten im Block auf einen Altslot gesetzt hat (nicht nur an der Blockgrenze).
    ensure_head_writable();

    // Slot zusammenbauen: Status erst NACH dem Inhalt schreiben (atomare Commit-Sequenz)
    Slot slot;
    memset(&slot, 0xFF, sizeof(slot));  // alles 1en damit Bit-Writes spaeter funktionieren
    slot.status      = STATUS_EMPTY;        // wird gleich von 0xFF auf 0xFE geflippt
    slot.reserved[0] = SLOT_LAYOUT_VERSION; // FIXES B.5: Layout-Marker
    slot.unix_s = row.unix_s;
    memcpy(slot.values, row.values, sizeof(slot.values));
    slot.valid_mask = 0;
    for (int i = 0; i < TELEM_FIELD_COUNT; i++) if (row.valid[i]) slot.valid_mask |= (1UL << i);

    // Inhalt schreiben (Bytes 1..127, Status bleibt vorerst 0xFF)
    esp_err_t e = esp_partition_write(s_part,
                                      slot_offset(s_head) + 1,
                                      ((uint8_t*)&slot) + 1,
                                      SLOT_SIZE - 1);
    if (e != ESP_OK) {
        syslog("STORE", "Append: Write Body fehlgeschlagen");
        return false;
    }

    // Commit: Status 0xFF → 0xFE
    if (!write_status(s_head, STATUS_WRITTEN)) {
        syslog("STORE", "Append: Status-Commit fehlgeschlagen");
        return false;
    }

    s_head = (s_head + 1) % s_total_slots;
    s_pending++;
    return true;
}

// Slot (aktuelles oder 22-Felder-Layout) → TelemetryRow. false = fremdes Layout.
static bool slot_to_row(const Slot& slot, TelemetryRow& out) {
    uint8_t layout = slot.reserved[0];
    bool legacy    = (layout == 0xFF || layout == SLOT_LAYOUT_LEGACY22);
    if (!legacy && layout != SLOT_LAYOUT_VERSION) return false;
    out.unix_s = slot.unix_s;
    if (legacy) {
        const SlotLegacy22& old = reinterpret_cast<const SlotLegacy22&>(slot);
        for (int i = 0; i < TELEM_FIELD_COUNT; i++) {
            bool in_old = i < LEGACY22_FIELDS;
            out.values[i] = in_old ? old.values[i] : 0.0f;
            out.valid[i]  = in_old && old.valid[i] != 0;
        }
    } else {
        memcpy(out.values, slot.values, sizeof(out.values));
        for (int i = 0; i < TELEM_FIELD_COUNT; i++) out.valid[i] = (slot.valid_mask >> i) & 1UL;
    }
    return true;
}

bool telem_store_peek(TelemetryRow& out) {
    if (!s_part || !s_mtx) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    if (s_pending == 0) { xSemaphoreGive(s_mtx); return false; }

    Slot slot;
    esp_err_t e = esp_partition_read(s_part, slot_offset(s_tail), &slot, SLOT_SIZE);
    if (e != ESP_OK) {
        // FIXES B.17: Read-Fehler → slot.* ist uninitialisiert. NICHT auf
        // Stack-Garbage hin tail vorrücken (würde eine valide Row überspringen).
        xSemaphoreGive(s_mtx);
        return false;
    }
    // FIXES B.5: fremdes Layout (Felderweiterung nach Flash) verwerfen statt als
    // verschobene Garbage-Telemetrie zu senden. 0xFF = unversionierter Alt-Slot
    // → als aktuelles Layout akzeptieren.
    bool layout_ok = slot.status == STATUS_WRITTEN && slot_to_row(slot, out);
    if (slot.status != STATUS_WRITTEN || !layout_ok) {
        // defensiv: kaputten/fremden Slot überspringen
        s_tail = (s_tail + 1) % s_total_slots;
        if (s_pending > 0) s_pending--;
        if (slot.status == STATUS_WRITTEN) syslog("STORE", "Peek: Slot mit fremdem Layout verworfen (B.5)");
        xSemaphoreGive(s_mtx);
        return false;
    }

    xSemaphoreGive(s_mtx);
    return true;
}

void telem_store_ack() {
    if (!s_part || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (s_pending == 0) { xSemaphoreGive(s_mtx); return; }

    // Status auf 0xFC setzen
    write_status(s_tail, STATUS_SENT);

    s_tail = (s_tail + 1) % s_total_slots;
    s_pending--;

    xSemaphoreGive(s_mtx);
}

uint32_t telem_store_pending() {
    return s_pending;  // lesender single-word access, ok ohne Lock
}

uint32_t telem_store_capacity() {
    return s_total_slots;
}

// ============================================================
//  1.6.3: Erneut senden ab Zeitpunkt
//  Gesendete Slots (0xFC) koennen ohne Erase nicht zurueck auf 0xFE. Darum
//  werden sie als neue Zeilen mit ihrem alten Zeitstempel hinten angehaengt.
//  Der Server schreibt sie unter demselben Zeitstempel; was schon da war,
//  wird nur ueberschrieben. Laeuft im Telemetrie-Task (telem_store_tick),
//  nicht im Web-Handler — der Scan liest die ganze Partition.
// ============================================================

static volatile bool s_rs_req  = false;
static uint32_t      s_rs_from = 0, s_rs_to = 0;
static char          s_rs_status[96] = "";

struct RsItem { uint32_t ts; uint32_t idx; };

static int rs_cmp(const void* a, const void* b) {
    uint32_t x = ((const RsItem*)a)->ts, y = ((const RsItem*)b)->ts;
    return x < y ? -1 : (x > y ? 1 : 0);
}

bool telem_store_request_resend(uint32_t from_unix, uint32_t to_unix) {
    if (!s_part || s_rs_req || to_unix <= from_unix) return false;
    s_rs_from = from_unix;
    s_rs_to   = to_unix;
    snprintf(s_rs_status, sizeof(s_rs_status), "laeuft");
    s_rs_req  = true;
    return true;
}

const char* telem_store_resend_status() { return s_rs_status; }

static void do_resend(uint32_t from, uint32_t to) {
    // Hoechstens ein Viertel des Puffers: die neuen Zeilen duerfen die alten,
    // noch zu kopierenden nicht ueberholen (Erase am Kopf).
    uint32_t max_items = s_total_slots / 4;
    RsItem* items = (RsItem*)malloc(max_items * sizeof(RsItem));
    uint8_t* sec  = (uint8_t*)malloc(SECTOR_SIZE);
    if (!items || !sec) {
        free(items); free(sec);
        snprintf(s_rs_status, sizeof(s_rs_status), "Fehler: kein Speicher");
        syslog("STORE", "Erneut senden: kein Speicher");
        return;
    }

    uint32_t n = 0;
    bool too_many = false;
    uint32_t num_sectors = s_total_slots / SLOTS_PER_SECTOR;
    for (uint32_t sc = 0; sc < num_sectors && !too_many; sc++) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        esp_err_t e = esp_partition_read(s_part, sc * SECTOR_SIZE, sec, SECTOR_SIZE);
        xSemaphoreGive(s_mtx);
        if (e != ESP_OK) continue;
        for (uint32_t k = 0; k < SLOTS_PER_SECTOR; k++) {
            const Slot* sl = (const Slot*)(sec + k * SLOT_SIZE);
            if (sl->status != STATUS_SENT) continue;
            if (sl->unix_s < from || sl->unix_s > to) continue;
            if (n >= max_items) { too_many = true; break; }
            items[n].ts  = sl->unix_s;
            items[n].idx = sc * SLOTS_PER_SECTOR + k;
            n++;
        }
        if ((sc & 31) == 31) vTaskDelay(1);
    }
    free(sec);

    if (too_many) {
        free(items);
        snprintf(s_rs_status, sizeof(s_rs_status), "Zeitraum zu gross (mehr als %u Zeilen)", (unsigned)max_items);
        syslog("STORE", "Erneut senden abgelehnt: Zeitraum zu gross");
        return;
    }
    qsort(items, n, sizeof(RsItem), rs_cmp);

    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) { vTaskDelay(10); continue; }
        Slot slot;
        TelemetryRow row;
        bool ok = esp_partition_read(s_part, slot_offset(items[i].idx), &slot, SLOT_SIZE) == ESP_OK
                  && slot.status == STATUS_SENT && slot.unix_s == items[i].ts
                  && slot_to_row(slot, row) && append_locked(row);
        xSemaphoreGive(s_mtx);
        if (ok) done++;
        if ((i & 15) == 15) vTaskDelay(1);
    }
    free(items);

    snprintf(s_rs_status, sizeof(s_rs_status), "%u Zeilen erneut eingereiht", (unsigned)done);
    char m[112];
    snprintf(m, sizeof(m), "Erneut senden: %u von %u Zeilen eingereiht (%lu..%lu)",
             (unsigned)done, (unsigned)n, (unsigned long)from, (unsigned long)to);
    syslog("STORE", m);
}

void telem_store_tick() {
    if (!s_rs_req || !s_part || !s_mtx) return;
    do_resend(s_rs_from, s_rs_to);
    s_rs_req = false;
}
