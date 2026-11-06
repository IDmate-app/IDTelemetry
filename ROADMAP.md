# Roadmap Stick — nächste Schritte

Stand: 2026-11-06, Firmware auf GitHub: 1.4.0 (`main`).

**Vorbereitet auf Branch `next-1.5` (nicht geflasht, nicht released):** Punkt 1
(Log-Fix), 0.6 (Build-Kennung), 2 bis auf die Formeln (Felder, Payload-Bits
20–22, Info-Telegramm, eigenes Polling von 0x0364/0x2613 mit Rohbytes).
IDmate liest das alles seit 1.2.255. Offen: Formeln in `decode_new_did()`
(`src/mod_telemetry.cpp`) und Punkt 5. Erst nach dem Firmware-Abgleich (0)
flashen.

Reihenfolge: erst **0** (wissen, was auf dem Stick läuft), dann **1** (Logs
abrufbar) und **5** (Funkloch-Hänger), dann **2–4** (neue Werte). Ohne 0 und 1
lässt sich 2 nicht entschlüsseln.

---

## 0. Firmware-Stand abgleichen

**Ergebnis (Flash-Abbild vom 2026-11-06, ausgewertet mit `tools/stick_dump.py`):**

- `FW_VERSION` im Image: **„1.2.0“** — trotzdem kein 1.2.0-Release-Stand.
- Basis ist der Zweig `gps/ubx-ack-baud-autodetect` (26.08., 1.2.0 + UBX-ACK,
  Baud-Erkennung) **plus** damals nicht committete Arbeit, die erst am 26.10.
  in `eab0a5d` (L1+L5) gelandet ist: `can listen`/`can selftest`,
  BUS_OFF-Recovery, GPS-Standby/-Powercycle/-Cache, Akku-Erkennung.
- Es fehlen **alle Phase-B-Fixes** (`FIXES.md` B.1–B.17), u. a.
  Slot-Layout-Kennung (B.5), MQTT-WiFi mit QoS1/PUBACK (B.4), begrenzter
  PLMN-Scan (B.12), RTC-Glitch-Filter, Konfig-Validierung.
- Ringpuffer: 22 Felder, Byte-Flags, Layout-Marker `0xFF` (unversioniert) —
  wird von `next-1.5` gelesen. Beim Auslesen 0 ungesendete Zeilen.
- Logs vollständig vorhanden (`sys.log` ~730 KB, `elm.log` ~250 KB,
  `sys.log.snap`), dazu viele Zeilen aus gelöschten Seiten.
- Bewertung: `next-1.5` (= 1.4.0 + Neues) ist ein großer Sprung gegenüber dem
  Stick, enthält aber alles, was im Image steckt. Rückweg: `app0.bin` aus dem
  Abbild.

**Problem:** Auf dem Stick läuft ein Stand, der nicht sicher einem Commit auf
GitHub entspricht (eine frühere Sitzung hat lokale Änderungen nicht gepusht).
Die Weboberfläche zeigt **keine** Firmware-Version an. Der Stick bleibt, bis
der Abgleich steht, auf seiner jetzigen Firmware (läuft seit ~3 Monaten
stabil).

**Workflow:** Schritt für Schritt in [`docs/LADEPAUSE.md`](docs/LADEPAUSE.md),
Zerlegen mit `tools/stick_dump.py` (Partitionen, Logs, Ringpuffer,
Commit-Vergleich über die Texte der Firmware).

1. **Sichern, ohne die Stick-Software zu benutzen:** vollständiges Flash-Abbild
   über USB am PC, gelesen vom ROM-Bootloader (die Firmware läuft dabei nicht
   und kann keine Logs leeren):

   ```bash
   esptool.py --chip esp32s3 read_flash 0 0x1000000 stick_full_JJJJ-MM-TT.bin
   ```

   Falls esptool den Stick nicht findet: BOOT-Taste halten, kurz RESET, BOOT
   loslassen, nochmal starten. Das Abbild (16 MB) enthält Firmware, Partitionen,
   **NVS mit Zugangsdaten (AES-Key, APN, WLAN)** und die SPIFFS-Partition mit
   `sys.log` / `elm.log`. → **Privat aufbewahren, nicht hochladen.**

2. **Teile herauslösen** (aus dem Abbild, offline):
   - Firmware: `app0` ab `0x10000`, 2 MB → nur dieser Teil wird verglichen.
     Nicht öffentlich weitergeben: ein privat gebauter Stand kann Werte aus
     `secrets.h` (AES-Key) einkompiliert haben.
   - Logs: SPIFFS ab `0x720000` (Full) bzw. `0x320000` (Lite) mit `mkspiffs -u`
     entpacken → `sys.log`, `elm.log`.
   - Zusätzlich, falls einfacher: Stick normal starten, serielles Terminal
     115200 Baud, `syslog` und `elmlog` eingeben (liest nur, löscht nicht).

3. **Version bestimmen:** Gesichert ist nur: Der Stand ist **nach August 2026**
   (L1+L5-GPS seit August). Dass die Weboberfläche keine Version zeigt, sagt
   nichts über das Alter (die Anzeige erscheint erst, wenn `/status` geladen
   ist; Ladeproblem am Handy möglich). **Kein Code wird auf einen vermuteten
   Stand hin geändert** — erst der Vergleich des Images. Das Kompilierdatum in `esp_app_desc_t` taugt
   bei Arduino nicht (stammt aus der vorgebauten Bibliothek). Entscheidend
   sind die Log-Texte im Image: `stick_dump.py --repo` vergleicht sie mit
   allen Commits; Log-Texte und `FW_VERSION`-String → welcher Stand (z. B. „Baudrate erkannt“ = GPS-Fix vom
   26.08., L1+L5-Texte = 1.4.0).

4. **Unterschiede finden:** Kandidaten-Commits bauen, Strings und Funktionen
   mit dem gesicherten Image vergleichen. Was im Image steckt, aber nicht in
   `main`, wird aus dem Verhalten/Strings nachgebaut oder bewusst verworfen.

5. **Schrittweise hochziehen:** vom gesicherten Stand aus in kleinen,
   einzeln getesteten Releases auf `main` + neue Features. Jedes Release mit
   Build-Kennung (unten). Rückweg jederzeit: gesichertes `app0` zurückflashen
   (`esptool.py write_flash 0x10000 app0_alt.bin`).

6. **Dauerhaft verhindern:** Build-Kennung in die Firmware.
   - PlatformIO-`extra_script` schreibt `git describe --always --dirty` als
     `FW_BUILD` in den Build (z. B. `1.4.0-3-gfd93023-dirty`).
   - Anzeigen auf der Weboberfläche (Kopfzeile), in `/status` und im
     Boot-Syslog.
   - Im MQTT-Telegramm (einmal pro Verbindung): IDmate zeigt Firmware und
     Build-Kennung des Sticks an und weiß, welches Telegramm-Format er spricht.
   - `-dirty` heißt: gebaut aus nicht committetem Code → sofort sichtbar.

---

## 1. Bug: System-Log und ELM-Log nicht abrufbar

**Ursache (im Code gefunden):**

- `/download-sys-log` und `/download-elm-log` machen einen „Snapshot per
  Rename“: die Log-Datei wird in `*.snap` **umbenannt** und dann gesendet.
  Danach schreibt der Stick in eine neue, leere Datei. Jeder Abruf leert also
  das Log, und der nächste Abruf überschreibt den alten Snapshot.
- Die Debug-Seite (`data/debug.html`) ruft genau diese Endpunkte für ihre
  Vorschau **automatisch** ab: System-Log alle 20 s, ELM-Log alle 15 s.
  Solange die Seite offen ist, wird das Log ständig „aufgefressen“. Der
  Download-Knopf liefert dann nur die Zeilen seit der letzten Vorschau, meist
  `# leer`.
- Das ELM-Log wird außerdem **nur** geschrieben, wenn in der Konfiguration
  „CAN / ELM Log“ (`log_can`) eingeschaltet ist. Standard ist **aus**
  (`LOG_CAN_ENABLED_DEFAULT false`).

**Fix:**

- Download nicht mehr destruktiv: unter `spiffs_lock` in eine Snapshot-Datei
  **kopieren** (in Blöcken), Original bleibt; dann den Snapshot senden.
- Vorschau der Debug-Seite über einen eigenen Endpunkt `…-tail?n=60`, der nur
  das Ende der Datei liest (`seek` auf die letzten ~8 KB), ohne umzubenennen.
- Debug-Seite: Hinweis, wenn `log_can` aus ist („ELM-Log ist ausgeschaltet —
  in der Konfiguration aktivieren“).
- Hinweis: `log_can` ist auf dem Stick an, das ELM-Log wird also geschrieben
  (Ring, 256 KB), aber genauso von der Debug-Vorschau geleert.

**Bis dahin (Workaround):**

- Debug-Seite **nicht** offen lassen; den Download-Link direkt aufrufen
  (`http://192.168.4.1/download-sys-log`), und nur einmal.
- Über USB am PC geht es ohne Verlust: serielles Terminal mit 115200 Baud
  (z. B. `pio device monitor`), dann `syslog`, `syslog 200` oder `elmlog`
  eingeben — die Befehle lesen die Datei, ohne sie umzubenennen.
- Für das ELM-Log vorher „CAN / ELM Log“ in der Konfiguration einschalten.

---

## 2. Feature: ABRP-Werte auch per MQTT an IDmate

**Prinzip (bleibt):** Der Stick liest den CAN-Bus selbst und beantwortet ABRP
aus seinem Telemetrie-Cache (`mod_elm327.cpp`, `DID_CACHE_MAP`). Für die elf
bekannten DIDs geht von ABRP aus **kein** Verkehr auf den Bus.

**Lücke:** ABRP fragt zwei DIDs, die der Stick nicht selbst pollt. Die gehen als
Ausnahme per CAN-Fallback durch:

| ECU | DID | Vermutung (ABRP zeigt) |
|---|---|---|
| FC0076 (Batterie) | `0x0364` | verbleibende Energie (SoE, z. B. 51 kWh) oder Klimaleistung |
| 746 (Klima) | `0x2613` | Innentemperatur (z. B. 24 °C) / Klimaleistung (1,1 kW) |

**Schritte:**

1. Rohbytes besorgen (braucht Punkt 1): `log_can` an, mit ABRP fahren,
   2–3 Screenshots der ABRP-Werteseite mit Uhrzeit, danach ELM-Log laden.
   Die Antworten stehen dort als `ELM UDS DID=0x0364` / `0x2613`.
2. Formeln aus Bytes ↔ ABRP-Werten ableiten.
3. Beide DIDs in die Poll-Tabelle (`mod_telemetry.cpp`) und in
   `DID_CACHE_MAP` → ABRP wird auch dafür aus dem Cache bedient, der
   CAN-Fallback entfällt.
4. Neue Payload-Bits **anhängen** (nie umnummerieren): z. B. `BF_SOE` (20),
   `BF_HVAC_KW` (21), `BF_CABIN_TEMP` (22). Payload-Version prüfen.
5. IDmate-Server: Decoder um die Bits erweitern (`_BIN_FIELDS_V2`), Felder
   `soe`, `hvac_kw`, `cabin_temp` in Influx, Dashboard, BMS-Lernen
   (SoE ist für die Anzeige-Modelle der wichtigste Wert).
6. Firmware **1.5.0**, Changelog, Release über den Workflow.

Datenvolumen: wenige Bytes pro Telegramm mehr, Werte nur bei Änderung
(Delta-Kompression bleibt).

---

**Stand 2026-11-06 (ELM-Log aus 1.5.1 + ABRP-Befehlsliste):** In ABRPs
Befehlsliste haben `220364` und `222613` **keine Gleichung** — ABRP fragt sie,
rechnet aber nichts daraus; sein SoE ist vermutlich SoC × Kapazität (`222AB2`,
`INT16(A:B)*50/1000`, identisch mit dem Stick). Dafür fragt ABRP alle 5 s
**`0x2AB8` an ECU 710** (nicht in der Liste, vermutlich eingebautes Profil):
`A:B:C:D` fällt beim Fahren gleichmäßig (3355 → 3141 in 13 min). Ab 1.5.2 pollt
der Stick 2AB8 selbst, bedient ABRP aus dem Cache und kalibriert den Faktor
über Spannung × Strom (`CAL2AB8` im Syslog). Steht der Faktor, wird 2AB8 → SoE
(Bit 20). 0x0364 (Werte 4–16) eher eine Leistung, 0x2613 (≈ 309–327) eher eine
Temperatur — weiter offen.

**Web-OTA: umgesetzt in 1.6.0** — `app1` (2 MB), `telem` 5 → 3 MB, SPIFFS und
NVS bleiben. Einmal per USB, danach Updates über die Konfig-Seite, mit
Absturzschutz (3 Abstürze → vorherige Firmware) und Rückweg-Knopf.

## 3. SoC-Formel: Stick und ABRP rechnen verschieden

- Stick (`0x028C`): `A · 0,4425 − 6,1947`
- ABRP (öffentliches MEB-Profil): `1,12 · A / 2,5 − 7,16` = `A · 0,448 − 7,16`
- Bei ~62 % macht das rund ein halbes Prozent. Welche näher an der Anzeige im
  Auto liegt, zeigt das BMS-Lernen in IDmate (Lernpunkte Anzeige ↔ CAN).
  **Nicht** vorschnell ändern: IDmates Kapazitätsmessung hängt an der
  bisherigen Formel.

---

## 4. Reichweite vom CAN-Bus

Der Stick liest die Reichweite (`0x0295`) schon mit; IDmate speichert sie seit
1.2.254 bei jedem BMS-Lernpunkt (`can_range`). Ist sie dauerhaft gleich der
Display-Reichweite, kann die manuelle Eingabe der km entfallen.

---

## 5. Bug: Start im Funkloch → LTE hängt bis zum Neustart

**Beobachtung:** Startet der Stick ohne Netz, bleibt LTE/MQTT oft hängen. Er
zeichnet weiter alle Zeilen auf und bedient ABRP, sendet aber nichts mehr bis
zum nächsten Neustart. Dann lädt er hunderte bis tausende Zeilen am Stück nach;
in IDmate fehlt solange der Verlauf der Fahrt.

**Log-Befund (gerettetes `sys.log`, Sept.–Nov. 2026) — die eigentliche Ursache:**

Nicht der Start im Funkloch, sondern **jeder Abriss während der Fahrt**:
`Publish fehlgeschlagen` → Reconnect → immer wieder „GPRS OK aber
MQTT-Connect fehlgeschlagen“ → PWRKEY-„Reset“ → danach durchgehend `reg=-1`
(Modem antwortet nicht = ausgeschaltet) → PLMN-Scans → nach **20–35 min**
Eskalations-Neustart. Direkt nach dem Neustart steht alles in ~20 s
(Registrierung, PDP, MQTT) — **ohne** Modem-Reset. Das wiederholt sich in
den Logs rund zehnmal; die Zeilen gehen nicht verloren, kommen aber erst nach
dem Neustart (oder nach dem nächsten Deep-Sleep-Wake).

Ursachen im Code (identisch in Stick-Stand und 1.4.0):

- `mqtt_connect()` konfigurierte MQTT nur **einmal** (`SMDISC`/`SMCONF` hinter
  `s_configured`). Nach einem Abriss wurde nie neu konfiguriert; der Neustart
  tut genau das.
- `modem_ensure_connected()` kehrt sofort zurück, wenn der PDP-Kontext
  „aktiv“ meldet — ein toter Kontext wird nie erneuert.
- Der PWRKEY-Impuls schaltet das laufende Modem **aus**.

Fix auf `next-1.5`: SMDISC + SMCONF bei jedem Connect; nach „GPRS OK aber
MQTT fehlgeschlagen“ PDP trennen; erste Eskalation = Funk neu aufbauen wie
nach dem Boot (Registrierung → PDP → MQTT) ohne PWRKEY und ohne ESP-Neustart;
PWRKEY erst danach, mit Wiedereinschalten.

„Keine SIM“ und „Kein Operator erreichbar“ kommen in den Logs **nicht** vor —
Kandidaten 1 und 2 unten sind bei diesem Stick nicht beobachtet (die Fixes
schaden nicht und bleiben).

**Ursprüngliche Analyse (Code, vor dem Log-Befund) — drei Kandidaten:**

1. **„Keine SIM“ ist endgültig (stärkster Verdacht für „bis zum Neustart“).**
   Jeder Netzsuch-Versuch endet im Auto-Modus (`COPS=0`), das Modem sucht dann
   selbst weiter. Der nächste Versuch beginnt mit `AT+CPIN?`; im Funkloch ist
   das Modem beschäftigt und antwortet „SIM busy“ oder gar nicht. Ein einziger
   Fehlschlag → `STATE_CHECK_SIM` → „Keine SIM · GPS-only Modus“ →
   `STATE_RUNNING` mit `s_sim_ok = false`. Die ganze MQTT-Eskalation hängt an
   `s_sim_ok` → **nie wieder ein Verbindungsversuch** bis zum Neustart. Zeilen
   werden weiter aufgezeichnet, ABRP läuft weiter — genau das Bild.
   *Fix:* 3 Versuche im Abstand von 5 s, bevor „Keine SIM“ gilt; danach im
   Betrieb alle 60 s neu prüfen und bei READY regulär suchen.
2. **Blindes Warten im Backoff.** Nach Fehlversuchen wartet der Stick 60 s,
   5 min, dann je 15 min, ohne nachzusehen, ob das Modem inzwischen selbst ein
   Netz gefunden hat. Ein Versuch dauert zusätzlich 3–8 min. Kurze Fahrt nach
   Start in der Tiefgarage → ganze Fahrt ohne Upload, nach dem nächsten Start
   alles am Stück. *Fix:* während des Backoffs alle 15 s den
   Registrierungsstatus lesen; erlaubtes Netz → sofort GPRS + MQTT (ohne neue
   Suche, die das Netz in Phase 0 wieder abmelden würde).
3. **PWRKEY-„Reset“ schaltet um.** Stufe 2 der Eskalation gibt einen
   PWRKEY-Impuls; beim SIM7080 schaltet der an ↔ aus. War das Modem an, ist es
   danach womöglich aus, und erst der nächste Reset schaltet es wieder ein.
   *Fix:* antwortet nach dem Impuls nichts, zweiter Impuls und bis 10 s auf AT
   warten, Echo wieder aus.

**Belege im geretteten `sys.log` suchen:**
`grep -n "Keine SIM\|SIM-Status unerwartet\|SIM nicht READY\|Kein Operator\|Modem-Reset\|REBOOT-LIMIT" sys.log`
— „Keine SIM · GPS-only“ nach einem Start ohne Netz bestätigt Kandidat 1.

**Zu prüfen (`mod_modem.cpp`):**

- Netzsuche mit Backoff (60 s → 5 min → 15 min, B.12): kommt der Zustand nach
  einem Fehlschlag wieder in die Suche, oder bleibt er in einem Zweig stehen?
- `modem_ensure_connected()` / PDP-Aufbau nach dem ersten Fehlschlag: wird die
  4-Stufen-Eskalation (MQTT-Reconnect → GPRS → Modem-Reset) auch erreicht, wenn
  **nie** eine Verbindung bestand?
- Belege aus dem geretteten `sys.log`: Zeilen `MODEM` nach einem Kaltstart ohne
  Netz („Kein Operator erreichbar“, Backoff, Eskalation).

**Ziel:** Wird das Netz wieder sichtbar, verbindet der Stick innerhalb weniger
Minuten und lädt nach, ohne Neustart. Test: Start im Funkloch (Tiefgarage),
dann ins Netz fahren.


---

## 6. Idee: Ladevorgang passiv mitlesen (nur zuhören, nie senden)

**Ausgangslage:** Beim Laden liest der Stick heute nichts. Er stellt bei
ausgeschaltetem Auto keine Anfragen, weil Senden auf den schlafenden Bus den
VW-Alarm auslösen kann. Ladekurven fehlen deshalb in IDmate.

**Idee:** Nicht sprechen, nur lauschen. Im TWAI-Modus `LISTEN_ONLY` sendet der
CAN-Controller nichts, auch keine ACK-Bits; er ist elektrisch stumm und kann
weder den Bus wecken noch einen BUS_OFF auslösen. (Im Normalmodus bestätigt
der Controller jeden Rahmen mit ACK — das ist bereits Senden.)

**Versuch 1 — Lesen bei externer Spannung (Laden, Auto steht):**
- Stick während des Ladens per USB am Laptop, serielles Terminal 115200 Baud.
- `can listen 60` (seit 1.4.0 im Code; ob die Stick-Firmware den Befehl hat,
  zeigt erst der Firmware-Abgleich, Punkt 0) → Zahl der Rahmen und Kennungen.
- Ergebnis notieren: kommt überhaupt Verkehr am OBD-Port an? Beim MEB sitzt
  der OBD-Port hinter dem Gateway; es kann sein, dass dort ohne Anfrage
  **gar nichts** zu hören ist. Dann ist die Idee am OBD-Port erledigt.
- Kommen Rahmen: Kennungen mit Häufigkeit und Beispiel-Bytes ins Syslog
  (heute zeigt der Befehl nur die ersten 8 Kennungen ohne Daten → erweitern:
  Histogramm aller Kennungen, je 3 Beispiel-Rahmen).

**Versuch 2 — Lesen ohne externe Spannung (niemand im Auto):**
- Nur wenn Versuch 1 Verkehr zeigt.
- Gleicher Befehl, ausschließlich `LISTEN_ONLY`, keine Telemetrie-Abfragen,
  kein ELM/ABRP-Verkehr in dieser Zeit.
- Prüfen: Alarm bleibt ruhig, Auto schläft normal ein, 12-V-Verbrauch des
  Sticks über die Ladezeit (Stick bleibt dafür wach statt Deep Sleep).

**Wenn beides klappt — Ladevorgang tracken:**
- Neuer Zustand „Laden passiv“: Auto aus + Bus aktiv → CAN in `LISTEN_ONLY`,
  Telemetrie-Poller und ELM-Fallback gesperrt (harte Sperre im Code, nicht nur
  Konfiguration).
- Aus den mitgehörten Rahmen Spannung, Strom, SoC und Leistung dekodieren
  (Kennungen erst aus Versuch 1 bestimmen), als normale Telemetrie-Zeilen
  puffern und senden → Ladekurve in IDmate.
- Abbruch: kein Verkehr mehr für N Minuten → Deep Sleep wie bisher.

**Grundsatz:** Kein Schritt, der sendet, solange das Auto aus ist. Jeder
Versuch zuerst mit Laptop und Terminal, erst danach in die Firmware.

---

## 7. Firmware-Update aus IDmate (ohne Laptop, ohne Datenklau)

**Voraussetzung:** Der Web-Upload aus 1.6.0 hat sich auf echter Hardware
bewährt (einmal hochladen, einmal „zurück zur vorherigen Firmware“ — noch
**vor** dem Schließen des Gehäuses).

Grundsatz: **Prüfen kostet keine SIM-Daten, Laden nur nach Zustimmung.**

1. **Hinweis „Update verfügbar“ — erledigt (IDmate 1.2.257).** Der IDmate-Server
   fragt alle 6 h selbst bei GitHub nach dem neuesten Release (Pre-Releases
   eingeschlossen) und vergleicht mit der Firmware, die der Stick im
   Info-Telegramm meldet. Dashboard: „Update X verfügbar → Release-Seite“.
   Kein Byte über die SIM. Abschaltbar mit `STICK_UPDATE_CHECK=0`.
2. **Installieren über WLAN zu Hause (Standard).** Knopf „installieren“ in
   IDmate → Befehl per MQTT (`…/ack`-Topic, das der Stick schon abonniert)
   mit Version, Größe und **SHA-256** (GitHub veröffentlicht sie je Asset).
   Der Stick merkt sich den Auftrag in NVS. Beim nächsten Kontakt mit dem
   Heim-WLAN lädt er die Datei (~1,5 MB, Sekunden) über IDmate als
   Zwischenstation, prüft SHA-256 beim Schreiben und installiert nur bei
   Übereinstimmung. Liegt ein genehmigter Auftrag an, schiebt er das
   Einschlafen bis zum Ende auf (eigener Akku). Danach Absturzschutz und
   Rückweg wie bei 1.6.0.
3. **Sofort über LTE-M (nur ausdrücklich).** Zweiter Knopf mit Hinweis „ca.
   1,5 MB über die SIM“ (Things Mobile grob 5–12 ct/MB → ~8–20 ct pro Update).
   Download über das HTTP des SIM7080G von IDmate, 1–3 min, gleiche Prüfungen.

Die Firmware ist ~1,5 MB (`idtelemetry-full-*.bin`), nicht 16 MB — die 16 MB
sind der ganze Flash mit Logs, Puffer und Konfiguration.
