# IDTelemetry — Fix-Liste (Audit 2026-06-12)

**Oberste Regel: Der Stick läuft produktiv im Fahrzeug — never change a running system.**
Nichts aus dieser Liste wird "mal eben" geflasht. Kategorien:

- **[REPO]** — Repo-/Doku-Arbeit, der Stick wird nicht angefasst. Jederzeit machbar.
- **[FLASH]** — Codefix, wirksam erst beim nächsten **geplanten** Flash (Maintenance-Fenster, Stick erreichbar, Rollback-Binary bereit).
- **[RISIKO]** — der Fix selbst berührt heikle Pfade (Modem-Power, Sleep, AP-Handling, Concurrency). Nur mit Testaufbau am Schreibtisch verifizieren, nie direkt ins Fahrzeug.

Audit-Umfang: komplette `src/` (10.391 Zeilen), `data/`-Web-UI, platformio.ini, partitions. Empfänger-Seite (IDMate) wurde gegengeprüft.

---

## PHASE A — Sofort, ohne den Stick anzufassen [REPO]

### [x] A.1 — Uncommitteter Wire-Protokoll-v2-Diff im Working Tree — committen!
- **Datei**: `src/mod_mqtt.cpp` + `src/mod_payload.cpp` (uncommitted diff)
- **Befund**: Der Working Tree enthält die v2-Änderung (kWh u16→u32 gegen 6553,5-kWh-Cap, SoC-Clamp 0..100 gegen u16-Wraparound bei negativen BMS-Werten, Version-Byte 0x02). **Empfängerseitig verifiziert**: IDMate dekodiert v1 **und** v2 (`_BIN_FIELDS_BY_VERSION = {0x01:…, 0x02:…}`, app.py:6516) — der Diff ist also kompatibel. Aber: uncommitted = ein versehentliches `git checkout` und die Arbeit ist weg; außerdem ist unklar dokumentiert, ob der laufende Stick v1 (HEAD) oder v2 (Working Tree) geflasht hat.
- **Fix**: Diff committen; im Commit-Text festhalten, welcher Stand aktuell auf dem Stick läuft. `mod_payload.h:47` Doku-Kommentar sagt noch `[0x01]` → auf v2 korrigieren (siehe A.2).
- **Aufwand**: 10 min

### [x] A.2 — Toter Code: Encoder-Duplikat + SPIFFS-Queue-Altlast entfernen
- **Datei**: `src/mod_mqtt.cpp:179-359` (`#if 0`-Block build_binary/encrypt_payload), `src/mod_telemetry.cpp:82-200` (`spiffs_q_*` — static, nirgends aufgerufen, durch mod_telem_store ersetzt), `src/mod_payload.h:47` (Wire-Format-Doku sagt 0x01, Code sendet 0x02)
- **Problem**: Der `#if 0`-Encoder wurde im v2-Diff **mitgepatcht** — die Leiche wird aktiv mitgepflegt und ist eine Drift-Falle. Die SPIFFS-Queue ist komplett tot.
- **Fix**: beide Blöcke löschen, Header-Kommentar auf v2 korrigieren. Reines Repo-Cleanup, ändert das Binary nicht funktional.
- **Aufwand**: 20 min

### [x] A.3 — partitions.csv: otadata ohne zweiten OTA-Slot — OTA faktisch unmöglich, undokumentiert
- **Datei**: `partitions.csv:8-9`
- **Problem**: `otadata` vorhanden, aber nur `app0/ota_0` — kein ota_1, keine factory. OTA-Update ist unmöglich; jede künftige Layout-Änderung = USB-Re-Flash mit Erase der telem-Partition (Backlog-Verlust). Irreführende Kombination.
- **Fix**: in README/partitions.csv dokumentieren: „kein OTA, USB-only" — oder langfristig 2×app-Layout planen (kostet 2 MB SPIFFS). Entscheidung dokumentieren, nichts flashen.
- **Aufwand**: 10 min (Doku)

### [x] A.4 — Stale Kommentare: „AT+CPOF" existiert nirgends im Code
- **Datei**: `src/mod_pmu.cpp:27`, `src/mod_modem.cpp:1179`
- **Problem**: Kommentare behaupten, das Modem sei „bereits via AT+CPOF abgeschaltet" — grep-verifiziert: kein `CPOF` im gesamten Projekt. Das Modem wird hart stromlos geschaltet (siehe C.3). Der nächste Leser trifft Entscheidungen auf falscher Kommentarlage — gleiche Falle wie die Influx-Typ-Kommentare in IDMate (dort FIXES 9.14, zwei reale Produktionsvorfälle).
- **Fix**: Kommentare korrigieren („hart per DC3, CPOF-Shutdown siehe FIXES C.3").
- **Aufwand**: 10 min

### [x] A.5 — Internes SIM7080-GPS ohne Fix-Validierung (latent) — dokumentieren
- **Datei**: `src/mod_modem.cpp:831-849`
- **Problem**: `getGPS()`-Ergebnis geht ohne 0,0-Filter, ohne HDOP-/Sat-Gate, ohne Settle-Zeit in `gps_update()`; Kursberechnung ohne Mindestdistanz → Zufallskurs im Stand. Aktuell **inaktiv** (externes M10-GPS aktiv, hat alle Gates: mod_gps_ext.cpp:170-179). Wird zur P2-Falle, sobald jemand auf internes GPS umschaltet oder das M10 ausfällt.
- **Fix**: jetzt nur Warnkommentar an die Stelle + diese Liste; Code-Fix (gleiche Gates wie ext. GPS) beim nächsten Flash mitnehmen.
- **Aufwand**: 10 min (Doku) / 30 min (Code, [FLASH])

### [ ] A.6 — WiFi-Upload: `https://` vermutlich nicht funktionsfähig — verifizieren
- **Datei**: `src/mod_wifi_upload.cpp:196, 233`
- **Problem**: `http.begin(url)` für https ohne WiFiClientSecure/CA — je nach Arduino-Core schlägt `begin()` still fehl; der Kommentar „handles both" ist ungeprüft. Wer einen https-Slot konfiguriert, lädt evtl. nie hoch — Bruch erst im Drain sichtbar.
- **Fix**: am Schreibtisch mit Test-ESP gegen https-Endpoint verifizieren; README-Hinweis „nur http:// bzw. mqtt:// für Upload-Slots", bis geklärt.
- **Aufwand**: 30 min Verifikation

### [x] A.7 — Systemzeit (`time(NULL)`) wird nie gesetzt — Test-Row trägt 1970
- **Datei**: `src/mod_wifi_upload.cpp:287`
- **Problem**: kein `settimeofday`/`configTime` im Projekt → Test-Row im WiFi-Test-Pfad hat 1970-Timestamp. **Produktivpfad ist sauber**: `telem_capture` verwirft Rows bei `rtc_unix_ms()==0` (mod_telemetry.cpp:356). Kehrseite dokumentieren: RTC-Zelle leer + kein GPS + kein LTE → still gar keine Telemetrie.
- **Fix**: Test-Row auf `rtc_unix_ms()` umstellen ([FLASH], 5 min); Verhalten „keine Zeit = keine Rows" im README dokumentieren.
- **Aufwand**: 10 min (Doku)

### [ ] A.8 — Beobachtungs-Punkte aus dem Audit (kein Code-Fix, Logs prüfen)
- **VBUS-Limit fix 2 A** (`mod_pmu.cpp:66`): an schwachen Fahrzeug-USB-Ports (500 mA) möglicher VBUS-Einbruch → Flapping, das die Fahrtende-Erkennung (mod_modem.cpp:934-952) triggert. → Logs auf VBUS-Flattern prüfen.
- **Gyro-INT HIGH vor Sleep** (`mod_sleep.cpp:293-301`): wird nur geloggt („GPIO3=HIGH!"), nicht behandelt — hängt der INT, gibt es eine Wake-Schleife. → Logs auf dieses Muster prüfen.
- **GSV-Zähler stale bei GN-Talker** (`mod_gps_ext.cpp:217-234`): nur Anzeige betroffen.
- **RMC-Zeitparse ohne ≥0-Check** (`mod_gps_ext.cpp:188-195`): praktisch durch Status-'A'-Gate abgedeckt.
- **ELM-ISO-TP `received`-Zähler heuristisch** (`mod_can.cpp:650, 676`): Reassembly trimmt korrekt — harmlos, nur notiert.
- **Aufwand**: 20 min Log-Sichtung

### [x] A.9 — Hartkodierte DE-Strings im config.html-JS (DE+EN-Regel)
- **Datei**: `data/config.html:335, 348-357, 377` (`confirm(...)`, `toast('MQTT-Host…')`, Status-Texte)
- **Problem**: nur Deutsch, alle anderen Strings laufen über `t()`. de.json/en.json selbst sind synchron (je 172 Keys).
- **Fix**: Keys ergänzen. Web-UI liegt in SPIFFS — Update erfordert Filesystem-Flash, also faktisch [FLASH]-Fenster; Vorbereitung im Repo jederzeit.
- **Aufwand**: 30 min

---

## PHASE B — Beim nächsten geplanten Flash [FLASH]

Reihenfolge = Priorität. B.1–B.4 sind die Pflicht-Kandidaten fürs nächste Fenster.

### [x] B.1 — Telem-Store: Append schreibt mid-block in nicht gelöschte Flash-Slots (P1, Datenverlust)
- **Datei**: `src/mod_telem_store.cpp:208-213` + `boot_scan()` Z. 148-158
- **Problem**: Erase wird nur bei `s_head % SLOTS_PER_SECTOR == 0` geprüft, und nur am Status des **ersten** Slots. `boot_scan()` kann head aber **mitten im Block** auf einen 0xFC-Slot setzen (Wrap-around-Backlog, Crash während Drain, abgebrochener Erase). Dann schreibt `esp_partition_write` in nicht gelöschte NOR-Zellen: Bits nur 1→0 möglich → Body wird mit Altdaten ver-UND-et, Status-Commit scheitert still → Row korrupt/verloren, `s_pending` driftet. Zusatz: Header-Doku (mod_telem_store.h:12) stimmt nicht mit dem Code überein; bei fast vollem Ring kann der Boundary-Erase ungesendete 0xFE-Slots wegwerfen ohne pending-Korrektur.
- **Fix**: vor jedem Write Status des Ziel-Slots prüfen; wenn ≠0xFF → zur nächsten Blockgrenze springen und erasen. **Vor dem Flash auf Testaufbau verifizieren** (Partition künstlich fragmentieren, Reboot, Drain).
- **Schwere**: P1 · **Aufwand**: ~1 h + Test

### [x] B.2 — Modem-AT-Handshake: Endlosschleife ohne Abbruch → Boot hängt, kein Sleep, Akku leer (P1)
- **Datei**: `src/mod_modem.cpp:1184-1194`
- **Problem**: `while (!s_modem.testAT(500)) { … retry = 0; }` hat keine Abbruchbedingung. PMU nicht gefunden (→ DC3 aus → Modem stromlos) oder Modem-Defekt → `modem_init()` aus `setup()` hängt unendlich, `loop()`/`sleep_update()` laufen nie → kein Deep Sleep, Akku bis Tiefentladung leer. Gerät wirkt gebrickt.
- **Fix**: Versuchslimit (~5 PWRKEY-Zyklen ≈ 30 s), danach `s_sim_ok=false`, Syslog-Fehler, GPS-only weiterbooten. Fix selbst risikoarm (5 Zeilen).
- **Schwere**: P1 · **Aufwand**: 15 min

### [x] B.3 — `WIFI_UP`-Task fehlt in der Sleep-Shutdown-Liste → Crash beim Einschlafen möglich (P1/P2)
- **Datei**: `src/mod_sleep.cpp:262-264` vs. `src/mod_wifi_upload.cpp:256ff`
- **Problem**: `task_names[]` enthält `"WIFI_UP"` nicht. `enter_deep_sleep()` ruft `esp_wifi_stop()` (Z. 329), während der WIFI_UP-Task noch lebt (prüft `g_shutdown` nur zwischen bis zu 15-s-Delays, kann in synchronem `WiFi.scanNetworks()` oder HTTP-POST stecken) → WiFi-Stack unterm Task weggezogen → Panic beim Einschlafen → Reboot → Wake-Boot-Schleife, die nachts Akku frisst. **FEATURE_WIFI_UPLOAD=1 ist in beiden Produktiv-Envs aktiv** (platformio.ini:63, 85). Symptom in Logs: PANIC nach Sleep-Versuch.
- **Fix**: `"WIFI_UP"` in `task_names[]` aufnehmen; im Task `g_shutdown` engmaschiger prüfen.
- **Schwere**: P1/P2 · **Aufwand**: 15 min

### [x] B.4 — WiFi-MQTT-Drain: QoS 0 + sofortiges Ack = stiller Datenverlust (P1)
- **Datei**: `src/mod_wifi_upload.cpp:221, 350-354`
- **Problem**: PubSubClient-`publish()` = QoS 0, kein PUBACK; `true` heißt nur „in TCP-Puffer geschrieben". Direkt danach `telem_store_ack()` → Row unwiderruflich weg. WiFi-Abbruch mittendrin (Hotspot-Reichweite!) = bis zu 20 Rows pro Zyklus still verloren. Der LTE-Pfad macht es korrekt (QoS 1 + PUBACK-Wait, mod_mqtt.cpp:83, 379, 395) — Asymmetrie zwischen den Transporten.
- **Fix**: PubSubClient kann kein QoS-1-Publish → entweder HTTP-POST als Drain bevorzugen oder Lib wechseln (z. B. espMqttClient).
- **Schwere**: P1 · **Aufwand**: mittel

### [x] B.5 — Telem-Store: kein Layout-Versions-Marker im Slot
- **Datei**: `src/mod_telem_store.cpp:49-56` (`reserved[3]` ungenutzt)
- **Problem**: wird `TELEM_FIELD_COUNT` je geändert, dekodiert der Boot-Scan nach dem Flash alte Slots im neuen Layout → verschobene Garbage-Werte gehen als gültige Telemetrie an IDMate (teils innerhalb der Plausibilitätsfenster). Der static_assert schlägt wegen Padding (128 B bleibt 128 B) nicht an.
- **Fix**: `reserved[0]` = Layout-Version (z. B. `TELEM_FIELD_COUNT`); bei Mismatch Slot verwerfen. **Muss vor der nächsten Felderweiterung drin sein.**
- **Schwere**: P2 · **Aufwand**: 30 min

### [x] B.6 — row_try_capture: lat/lon aus zwei verschiedenen GPS-Fixes
- **Datei**: `src/mod_telemetry.cpp:301-302` (+ Direktzugriffe Z. 327, 346, 389)
- **Problem**: `gps_lat()`/`gps_lon()` sind getrennte Spinlock-Abschnitte; dazwischen kann `gps_update()` (anderer Core) laufen → lat von Fix A, lon von Fix B (~28 m Versatz bei 100 km/h, 1-Hz-GPS). Das eigens gebaute `gps_snapshot()` wird hier nicht benutzt.
- **Fix**: einmal `GpsSnapshot s = gps_snapshot();` am Funktionsanfang, nur daraus lesen.
- **Schwere**: P2 · **Aufwand**: 15 min

### [x] B.7 — Payload: BF_BATT_DEV nimmt Live-Wert statt Row-Wert → falsche Zeitreihe beim Backlog-Drain
- **Datei**: `src/mod_payload.cpp:101-104`
- **Problem**: Encoder ruft beim Publish `pmu_batt_pct()` live auf statt `row.values[TELEM_BATT_DEVICE]`. Beim Nachsenden eines Backlogs (Funkloch, Store-Drain — bis 40k Rows) bekommt jede historische Row den Akku-% vom **Sendezeitpunkt**.
- **Fix**: Row-Wert kodieren, Live-Wert nur als Fallback. Wire-Format unverändert, kein Server-Touch.
- **Schwere**: P2 · **Aufwand**: 10 min

### [x] B.8 — telem_persist_to_spiffs: SPIFFS-Lock-Leak auf zwei Fehlerpfaden
- **Datei**: `src/mod_telemetry.cpp:931, 944-947`
- **Problem**: nach `spiffs_lock(2000)` kehren der Mutex-Fehlerpfad und der `!f`-Pfad ohne `spiffs_unlock()` zurück → alle weiteren SPIFFS-Operationen im Sleep-Pfad laufen in Lock-Timeouts; genau in diesem Fenster gehen die zu persistierenden RAM-Rows verloren (Store-Partition fängt das meiste ab, daher nicht P1).
- **Fix**: `goto cleanup`-Muster oder unlock vor beiden returns.
- **Schwere**: P2 · **Aufwand**: 10 min

### [x] B.9 — Kein Unterspannungs-Schutz → Tiefentladung bei Gyro-Wake-Zyklen
- **Datei**: `src/mod_pmu.cpp` (gesamt), `src/mod_sleep.cpp`
- **Problem**: keine Low-Battery-Schwelle. Stick ausgebaut/Transport: Gyro weckt wiederholt per EXT1, jeder Wake = Minuten Netzsuche bei vollem Sendestrom → Akku zyklisch bis zur AXP2101-Hardware-Abschaltung leer (schlecht für die Zelle; danach RTC_DATA weg).
- **Fix**: in `sleep_update()`/Boot: `pmu_batt_pct() < N && !pmu_is_vbus_in()` → sofortiger Deep Sleep ohne Modem-Start; unter kritischer Schwelle nur VBUS-Wake.
- **Schwere**: P2 · **Aufwand**: 30 min

### [x] B.10 — PMU defekt ⇒ Schlaf-Wach-Schleife während der Fahrt
- **Datei**: `src/mod_sleep.cpp:129-144` + `src/mod_pmu.cpp:102-105`
- **Problem**: bei `!s_pmu_ok` liefert `pmu_is_vbus_in()` immer false → nach 5 min Uptime ist `vbus_sleep` true, unabhängig von Gyro-Bewegung → Gerät schläft mitten in der Fahrt ein, Gyro weckt sofort → Dauerschleife, Telemetrie-Lücken, Modem-Boot-Storm.
- **Fix**: bei `!s_pmu_ok` den VBUS-Sleep-Pfad deaktivieren oder konservativ `true` zurückgeben (wie Lite-Stub mod_pmu.h:34).
- **Schwere**: P2 · **Aufwand**: 15 min

### [x] B.11 — rtc_do_sync übernimmt DS1307-I2C-Glitches ungeprüft → falsche Timestamps bis 60 s
- **Datei**: `src/mod_rtc.cpp:178-199`
- **Problem**: der 60-s-Sync übernimmt h/m/s ohne Plausibilisierung (nur `y >= 2024` für Unix-Teil). mod_gps_ext.cpp:42 dokumentiert real beobachtete Glitches („Jahr 2093, Monat 25"). Glitch mit plausiblem Jahr, kaputter Stunde → `s_ref_unix` verschoben → bis 60 s lang falsche Row-Timestamps (Empfänger sortiert/filtert nach Zeit!). Die Validierung aus `inject_time()` (mod_gps_ext.cpp:44-49) fehlt hier.
- **Fix**: gleiche Feld-Plausibilisierung + Sprung-Erkennung (Abweichung > x s gegen `s_ref_unix + elapsed` → zweites Lesen zur Bestätigung).
- **Schwere**: P2 · **Aufwand**: 30 min

### [x] B.12 — Netzsuche ohne wachsenden Backoff → Dauerscan-Schleife in Funkloch-Lage mit VBUS
- **Datei**: `src/mod_modem.cpp:798-804`
- **Problem**: bei `try_register()`-Fehlschlag fix 60 s warten, dann volle Runde (inkl. COPS=? bis 3 min). `scan_fail_count` wird gezählt, nie genutzt. Tiefgarage/Laden über Nacht mit VBUS: Modem scannt stundenlang im Kreis. (Ohne VBUS greift der 120-s-Sleep-Fallback — Akku-Fall ist abgedeckt.)
- **Fix**: exponentieller Backoff (60 s → 5 min → 15 min) auf Basis `scan_fail_count`; COPS=? max. 1×/h.
- **Schwere**: P2 · **Aufwand**: 30 min

### [x] B.13 — DOM-XSS über WiFi-SSID in der Debug-Seite
- **Datei**: `data/debug.html:397`
- **Problem**: SSID aus `/wifi/scan-result` ungeescaped ins `innerHTML` (Log-Renderer nutzen `escE`/`escI`, hier fehlt es). Nachbar-SSID `<img src=x onerror=…>` führt JS im Browser des Besitzers aus — in Kombination mit C.1 (Secrets/OTA offen) ein echter Exfiltrations-Vektor.
- **Fix**: gleiche Escape-Funktion anwenden. (Web-UI in SPIFFS → braucht Filesystem-Flash.)
- **Schwere**: P2 · **Aufwand**: 10 min

### [x] B.14 — cfg_save_json ohne jede Eingabe-Validierung (inkl. AES-Key-Hex-Check)
- **Datei**: `src/mod_config.cpp:135-213`, Folge u. a. `src/mod_web.cpp:77`, `src/mod_payload.cpp:17-31`
- **Problem**: (a) `ap_pass` < 8 Zeichen → `WiFi.softAP()` startet ein **offenes** Netz. (b) `sim_pin` beliebiger String → PUK-Sperr-Risiko. (c) `mq_port` 0 erlaubt. (d) **AES-Key**: nur Längen-Check ≥64; `hex_nibble()` mappt Nicht-Hex auf 0, All-Zero-Beispiel-Key wird still akzeptiert → verschlüsselt anstandslos, Server kann nie entschlüsseln = **stiller Totalverlust der Telemetrie** ohne sichtbaren Fehler am Stick.
- **Fix**: serverseitige Validierung (ap_pass 8-63, sim_pin `^\d{4,8}$` oder leer, port 1-65535, aes_key exakt 64 Hex + nicht all-zero → sonst Syslog-Warnung + Fallback auf SECRET_AES_KEY, URL-Schema-Check).
- **Schwere**: P2 · **Aufwand**: 45 min

### [x] B.15 — cfg_to_json: `buf[1200]` zu klein → abgeschnittenes JSON legt Config-Seite UND i18n lahm
- **Datei**: `src/mod_config.cpp:216, 250`
- **Problem**: Summe der Feld-Maxima (ix_token 256, aes_key 128, upload_url ×2 je 128, diverse 64er) + Keys/Quotes > 1200. Bei real befüllter Vollkonfig liefert `/api/config` abgeschnittenes JSON → Config-Seite lädt nicht und `i18n.js` schlägt auf **allen** Seiten fehl.
- **Fix**: Puffer ≥2048 oder `serializeJson(doc, String)`.
- **Schwere**: P2 · **Aufwand**: 10 min

### [x] B.16 — GPS-Position leakt in `X-GPS-Location`-Header auf jeder Response
- **Datei**: `src/mod_headers.cpp:13-14`
- **Problem**: Live-Koordinaten an jeder Response von `/status`, `/api/*`, `/gps` — ohne Auth. Wer den AP erreicht, liest die exakte Fahrzeugposition.
- **Fix**: Header entfernen; Position nur über dedizierten (künftig authentifizierten, C.1) Endpoint.
- **Schwere**: P2 · **Aufwand**: 10 min

### [x] B.17 — Hygiene-Sammelposten (alle P3, beim nächsten Flash mitnehmen)
- `src/mod_telemetry.cpp:432` — `s_cap_ms = 0` außerhalb des `s_capture_mtx` (Kommentar Z. 64-66 beschreibt genau diesen Race) → Doppel-Row mit falschem Trigger-Grund möglich. 15 min
- `src/mod_telem_store.cpp:252-259` — uninitialisierter `slot.status` bei `esp_partition_read`-Fehler → tail-Advance auf Stack-Garbage → Row übersprungen. 5 min
- `src/mod_mqtt.cpp:173` — `resp.indexOf("1")` als SMSTATE-Check matcht jede ‚1' in URCs/Echos → falsch-positives „connected", verzögerte Eskalation. Fix: `indexOf("+SMSTATE: 1")`. 5 min
- `src/main.cpp:59-66` — `snprintf("%.6f")` innerhalb `portENTER_CRITICAL` (Cross-Core-Spinlock + deaktivierte Interrupts) → Latenz/Lock-Risiko; vorher formatieren, unter Lock nur memcpy. 10 min
- `src/mod_headers.cpp:17` — `pmu_update()` (I2C) bei jedem HTTP-Response im async_tcp-Task; nur gecachten Wert lesen. 5 min
- `src/main.cpp:319-330` — Debug-Serial-AT-Kommandos (`at …`, `mqtt drop`) kollidieren mit MODEM-Task-UART; vor Raw-AT `modem_pause_task()` erzwingen. 15 min
- `src/mod_modem.cpp:38-39, 996` — Reboot-Limiter: Fenster ohne Zeitsync läuft nie ab (failsafe-richtig), aber `s_wd_reboot_count` überlebt Deep Sleep → nächste Fahrt überspringt grundlos Präferenz-Provider. Counter beim normalen Sleep nullen. 10 min
- `src/mod_telemetry.cpp:527, 1167` — `TELEM_KWH_CHARGED` mit `plausible(v, 0, 99999999)` faktisch ungedeckelt → korrupter 0x1E32-Frame wird dauerhaft gespeichert+gesendet; realistische Obergrenze setzen. 5 min
- `data/config.html:410, 446` + `src/mod_config.cpp` — `mod_compass`-Checkbox wird gesendet, aber nie gespeichert (Key existiert nicht) → Einstellung wirkungslos. 10 min
- `src/mod_payload.cpp:53` — Heading ohne Clamp (`fmodf(h,360)` defensiv); GPS-float-Quantisierung ~0,4 m (bewusst lassen oder `lround`); `s_pub_count` u16-Wrap (kosmetisch). je 5-10 min

---

## PHASE C — Nur mit Testaufbau [RISIKO]

Diese Fixes berühren Modem-Power, Sleep-Pfade, AP-Handling oder Cross-Core-Concurrency. Ein Fehler hier macht aus einem laufenden System ein totes. **Erst am Schreibtisch-Stick verifizieren, nie direkt ins Fahrzeug.**

### [ ] C.1 — Webserver/AP hat keinerlei Authentifizierung — Secrets, OTA und CAN-TX offen (P1, der eine große Hebel)
- **Dateien**: `src/mod_web.cpp:623-635` (`/api/config` GET+POST), `:661-690` (`/api/ota`), `:130-154` (`/send`), `src/mod_config.cpp:215-252` (`cfg_to_json`), `data/i18n.js:55`
- **Problem** (drei Ausprägungen desselben Defizits):
  1. **Secrets im Klartext**: `GET /api/config` liefert ungeschützt `sim_pin`, `mq_pass`, `apn_pass`, `ix_token`, **`aes_key`**, `sta_pass` — und `i18n.js` ruft das bei **jedem Seitenaufruf** ab. Wer den AP erreicht (Passwort ist dokumentiert/ableitbar), kann die gesamte Backend-Telemetrie entschlüsseln und fälschen.
  2. **OTA ohne Auth/Signatur**: `POST /api/ota` flasht jede .bin und rebootet → vollständige Geräteübernahme inkl. CAN-Zugang zum Fahrzeug.
  3. **CAN-Injektion**: `POST /send` schickt frei wählbare Frames auf den Fahrzeugbus; einziges Gate ist `can_tx_allowed()` = VBUS + Bewegung — also **genau während der Fahrt** freigeschaltet.
- **Verschärft durch**: C.4 (AP läuft bei VBUS die ganze Fahrt) und B.13 (XSS als Fernzugriffs-Vektor auf die Seiten).
- **Fix-Skizze**: Session-/Basic-Auth vor allen `/api/*`+Config-Routes; `cfg_to_json` maskiert Passwörter (`"***"`); Sprache über eigenen secrets-freien `/api/lang`-Endpoint; `/send` per Default deaktivieren (nur Diagnose-Modus); OTA mindestens hinter Auth.
- **Warum [RISIKO]**: Auth kann den Besitzer aussperren; AP-Handling hat eine bekannte ipc0-PANIC-Geschichte (siehe C.4). Lockout-Fallback (z. B. Boot-Taste = Auth-Reset) einplanen.
- **Schwere**: P1 · **Aufwand**: mittel-hoch + Testaufbau

### [ ] C.2 — Eskalations-Stufe „Modem-Reset" resettet evtl. gar nicht — und re-initialisiert nie
- **Datei**: `src/mod_modem.cpp:1042-1059`, `:74-80`
- **Problem**: (a) `modem_pwrkey_pulse()` hält PWRKEY 1,0 s — SIM7080G braucht für Power-**Off** ≥ 1,2 s → der „Reset" eines hängenden Modems ist möglicherweise ein No-op. (b) Falls doch resettet: danach nur `testAT`, **kein `ensure_sim_ready()`** (PIN-SIM bleibt gesperrt) und kein `configure_radio()` → mit PIN-SIM können Stufe 2+3 nie gelingen → bis zu ~3 unnötige Full-Reboots pro Funkloch-Episode.
- **Fix**: Reset deterministisch per `AT+CFUN=1,1`; danach `ensure_sim_ready()` + `configure_radio()`.
- **Schwere**: P2 · **Aufwand**: mittel + Testaufbau (Power-Pfad!)

### [ ] C.3 — Modem wird vor Deep Sleep hart stromlos geschaltet (kein CPOF)
- **Datei**: `src/mod_sleep.cpp:314` (`pmu_set_modem_power(false)`)
- **Problem**: kein Netz-Detach, abrupter VDD-Verlust; SIMCom warnt vor NVRAM-/Flash-Korruption bei hartem Power-Cut. Bei 2+ Sleep-Zyklen/Tag über Monate ein reales Risiko. Kommentare behaupten fälschlich CPOF (siehe A.4).
- **Fix**: vor DC3-Off `AT+CPOF` senden, 2 s auf „NORMAL POWER DOWN" warten (Timeout-Fallback: trotzdem abschalten).
- **Schwere**: P2 · **Aufwand**: 30 min + Testaufbau (Sleep-Pfad!)

### [ ] C.4 — AP bleibt im Fahrbetrieb dauerhaft offen
- **Datei**: `src/mod_web.cpp:376` (`if (vbus) return;`)
- **Problem**: solange VBUS anliegt, greift der 2-min-AP-Timeout nie → AP läuft die gesamte Fahrt = maximales Angriffsfenster für C.1. **Achtung**: das ist ein bewusster Workaround für eine ipc0-PANIC beim AP-Stop — nicht naiv „fixen".
- **Fix**: erst die PANIC-Ursache am Testaufbau reproduzieren/lösen, dann AP-Timeout auch bei VBUS (z. B. 10 min nach letztem Client).
- **Schwere**: P2 · **Aufwand**: mittel + Testaufbau

### [ ] C.5 — Gemeinsamer I2C-Bus (RTC + MPU-6050) ohne Mutex
- **Datei**: `src/mod_gyro.cpp:130-217` (gyro_task, alle 100 ms) vs. `src/mod_rtc.cpp:90` (aufgerufen aus dem AsyncWebServer-`/status`-Handler, mod_web.cpp:225)
- **Problem**: beide nutzen `Wire` (GPIO45/21) ohne Lock; gleichzeitiger Zugriff aus zwei Tasks kann Transaktionen verschränken → korrupte Gyro-Reads (fließen in `TELEM_GYRO_G` und ins CAN-TX-Gate!) bzw. falsche RTC-Reads.
- **Fix**: globaler I2C-Mutex um alle Wire-Transaktionen (gyro + rtc + ggf. compass).
- **Schwere**: P2 · **Aufwand**: mittel + Testaufbau (Timing!)

### [ ] C.6 — `is_parked` über exakten Byte-Vergleich `d[1] == 8`
- **Datei**: `src/mod_telemetry.cpp:507, 1172` (ELM-Pfad konsistent: mod_elm327.cpp:262)
- **Problem**: setzt die ECU je zusätzliche Bits in dem Byte, wird „geparkt" nicht mehr erkannt. Nur am realen Bus verifizierbar — deshalb [RISIKO]/Beobachtung, kein blinder Fix.
- **Fix**: Bit-Maske statt Exakt-Vergleich, nachdem am Fahrzeug geloggt wurde, welche Werte real auftreten.
- **Schwere**: P3 · **Aufwand**: 10 min Code + Bus-Beobachtung

---

## Positiv verifiziert (kein Handlungsbedarf)

- millis()-Overflow: alle Vergleiche subtraktionsbasiert; täglicher Sleep-Reset entschärft zusätzlich
- LTE-MQTT: QoS 1 mit PUBACK + Peek/Ack (löscht erst nach bestätigtem Versand) — sauber
- PKCS7-Padding korrekt; IV aus esp_random zur Laufzeit mit aktivem RF = echte HW-Entropie
- Plausibilitätsfenster decken alle u16-Felder außer kWh (B.17) ab; Temperatur i8 reicht für −40..80
- ELM-Cache-Encode ist sauberer Inverse der CAN-Decode-Formeln (Vorzeichen korrekt)
- ISO-TP-Längen gegen `max_resp` geklemmt — keine Pufferüberläufe; Bus-Off-Recovery vorhanden
- Flash-Wear telem-Partition: Ring über 1280 Blöcke → rechnerisch unkritisch
- Zeitquellen konsistent UTC (CNTP tz=0, GPS-UTC); keine 1970-Telegramme im Produktivpfad
- mod_logs loggt keine Secrets; de.json/en.json synchron (172 Keys)
- secrets.h korrekt in .gitignore, nicht im Git-Index

---

## Empfohlene Reihenfolge

1. **Jetzt (Stick läuft weiter, Null Risiko)**: A.1 (v2-Diff committen!) → A.2–A.9
2. **Nächstes geplantes Flash-Fenster** (ein Binary, vorher Testaufbau für B.1): B.1 → B.2 → B.3 → B.4, dazu die billigen B.5–B.8 und den B.17-Sammelposten
3. **Schreibtisch-Projekt mit Test-Stick**: C.1 (Web-Auth — der eine große Hebel), danach C.2–C.5
4. **Beobachten statt fixen**: A.8-Punkte, C.6

Merksatz fürs Fenster: erst Binary mit Rollback-Plan (alter Build als .bin gesichert), dann flashen — die Kuh gibt auch nach dem Fenster noch Milch.
