# Roadmap Stick — nächste Schritte

Stand: 2026-11-06, Firmware auf GitHub: 1.4.0 (`main`).

Reihenfolge: erst **0** (wissen, was auf dem Stick läuft), dann **1** (Logs
abrufbar) und **5** (Funkloch-Hänger), dann **2–4** (neue Werte). Ohne 0 und 1
lässt sich 2 nicht entschlüsseln.

---

## 0. Firmware-Stand abgleichen

**Problem:** Auf dem Stick läuft ein Stand, der nicht sicher einem Commit auf
GitHub entspricht (eine frühere Sitzung hat lokale Änderungen nicht gepusht).
Die Weboberfläche zeigt **keine** Firmware-Version an. Der Stick bleibt, bis
der Abgleich steht, auf seiner jetzigen Firmware (läuft seit ~3 Monaten
stabil).

**Workflow:**

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
   - Firmware: `app0` ab `0x10000`, 2 MB → nur dieser Teil wird verglichen
     und darf weitergegeben werden.
   - Logs: SPIFFS ab `0x720000` (Full) bzw. `0x320000` (Lite) mit `mkspiffs -u`
     entpacken → `sys.log`, `elm.log`.
   - Zusätzlich, falls einfacher: Stick normal starten, serielles Terminal
     115200 Baud, `syslog` und `elmlog` eingeben (liest nur, löscht nicht).

3. **Version bestimmen:** Im App-Image steht die ESP-IDF-Beschreibung
   (`esp_app_desc_t`: Kompilierdatum/-zeit, IDF-Version) und alle Log-Texte der
   Firmware. Kompilierdatum → welche Commits es damals gab; Log-Texte und
   `FW_VERSION`-String → welcher Stand (z. B. „Baudrate erkannt“ = GPS-Fix vom
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

