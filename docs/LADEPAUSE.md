# Ladepause: Stick sichern (Firmware-Abgleich, ROADMAP 0)

Ziel: Firmware **und** Logs des Sticks sichern, **ohne** etwas zu verändern.
Es wird nichts geflasht. Danach läuft der Stick mit seiner bisherigen Firmware
weiter.

Dauer: etwa 10 Minuten (Lesen des Flash ~4 min).

## Vorher

- Die **Debug-Seite des Sticks nicht öffnen.** Sie ruft die Logs alle 15–20 s
  ab, und die laufende Firmware leert sie dabei (ROADMAP 1).
- Optional, nur zum Ansehen: Die Konfig-Seite zeigt die Version klein und grau
  neben „TS.CONFIG“, sobald `/status` geladen ist. Fehlt sie, kann das am
  Laden auf dem Handy liegen; über den Stand sagt das nichts. Den bestimmt nur
  der Vergleich des Images (Schritt 2).
- PC: Python 3 und esptool (`pip install esptool`).

## 1. Flash-Abbild lesen (sichert alles)

Stick aus dem OBD-Port, per USB-C an den PC.

```bash
esptool.py --chip esp32s3 --baud 921600 read_flash 0 0x1000000 stick_full_2026-11-06.bin
```

- Port wird automatisch gesucht; sonst `--port COM5` (Windows) bzw.
  `--port /dev/ttyACM0` (Linux).
- Findet esptool den Stick nicht: **BOOT** halten, kurz **RESET**, BOOT
  loslassen, Befehl wiederholen.
- esptool startet den Stick dabei in den ROM-Bootloader. Die Firmware läuft
  nicht, kann also nichts löschen. Zeilen, die nur im RAM lagen (die letzten
  Sekunden), können fehlen.

**Prüfen, ob das Abbild sauber gelesen wurde** (Firmware-Teil ein zweites Mal
lesen und vergleichen):

```bash
esptool.py --chip esp32s3 --baud 921600 read_flash 0x10000 0x200000 app0_check.bin
python3 -c "import sys;a=open(sys.argv[1],'rb').read()[0x10000:0x210000];b=open(sys.argv[2],'rb').read();print('gleich' if a==b else 'UNTERSCHIEDLICH - nochmal lesen')" stick_full_2026-11-06.bin app0_check.bin
```

Das Abbild enthält **NVS mit AES-Key, APN und WLAN-Passwörtern** →
privat aufbewahren, nicht hochladen, nicht ins Repo.

## 2. Zerlegen (offline)

Im IDTelemetry-Checkout (Branch `next-1.5`, dort liegt das Skript):

```bash
python3 tools/stick_dump.py stick_full_2026-11-06.bin --repo .
```

Ergebnis in `stick_full_2026-11-06_teile/`:

| Datei | Inhalt | weitergeben? |
|---|---|---|
| `app0.bin` | Firmware des Sticks | nur in der Sitzung, **nicht öffentlich**: ein privat gebauter Stand kann Standardwerte aus `secrets.h` (AES-Key, APN) einkompiliert haben |
| `app_info.txt` | Version, Build-Kennung, Merkmale | ja |
| `strings.txt` | Texte der Firmware | ja |
| `spiffs/sys.log`, `spiffs/elm.log` | Logs | nur in der Sitzung, **nicht ins Repo** (Positionen, Netze) |
| `telem.txt` | Ringpuffer: gesendet/ungesendet, Layout | ja |

`mkspiffs` fehlt? `pio pkg install -g -t platformio/tool-mkspiffs`, oder das
Skript schneidet die Syslog-Zeilen als Notlösung heraus.

Für den Abgleich reichen `app0.bin`, `app_info.txt`, `telem.txt` und die beiden
Logs. Das Skript vergleicht die Texte der Firmware mit allen Commits. Die Historie
vor dem öffentlichen Start ist auf GitHub nicht mehr vorhanden; den Vergleich
mit ihr mache ich in der Sitzung. Dafür `app0.bin` und `app_info.txt` (oder das
ganze `_teile`-Verzeichnis) in die Sitzung geben, nicht ins Repo.

## 3. Stick zurück ins Auto

Unverändert wieder einstecken. **Never change a running system** — geflasht
wird erst, wenn klar ist, was auf dem Stick läuft.

## 4. Für die Formeln (SoE, Klima, Innentemperatur)

Läuft nebenher auf der nächsten Fahrt, mit der **alten** Firmware:

1. In der Konfiguration „CAN / ELM Log“ an (ist an).
2. Mit ABRP fahren; **2–3 Screenshots** der ABRP-Werteseite (SoE, Klima kW,
   Innentemperatur), Uhrzeit muss sichtbar sein.
3. Danach Logs sichern wie oben (Abbild) oder einmalig
   `http://192.168.4.1/download-elm-log` (die alte Firmware leert es dabei,
   also nur einmal und vorher nicht die Debug-Seite öffnen).
4. Die Antworten stehen im ELM-Log als `ELM UDS DID=0x0364` und `0x2613`.

Aus Bytes ↔ Screenshot entstehen die Formeln, die in `decode_new_did()`
(`src/mod_telemetry.cpp`) eingetragen werden. Alles andere (Felder,
Payload-Bits 20–22, IDmate-Anzeige) ist schon fertig.

## Später: 1.5.0 flashen (nicht in dieser Ladepause)

- Nur `app0` wird geschrieben; NVS (Zugangsdaten), SPIFFS (Logs) und der
  Ringpuffer bleiben. Ungesendete Zeilen der alten Firmware werden gelesen
  (Layout 22 Felder bzw. unversioniert).
- **Rückweg** jederzeit, exakt der alte Stand:

  ```bash
  esptool.py --chip esp32s3 write_flash 0x10000 stick_full_2026-11-06_teile/app0.bin
  ```

- Nach dem Flash: `version` im seriellen Terminal, Kopfzeile der Weboberfläche
  und IDmate-Dashboard („Stick-Firmware“) zeigen dieselbe Build-Kennung.
