# Bereinigung — Offene Altlasten

Stand: 2026-08-26

## Offene Punkte

### Entscheidung: drei GPS-Quellen oder Festlegung auf den DAN-F10N?

`cfg_gps_src()` kennt heute drei Wege, und alle drei werden mitgepflegt:

| Wert  | Quelle                     | Code                                    |
| ----- | -------------------------- | --------------------------------------- |
| `ext` | u-blox DAN-F10N an UART2   | `mod_gps_ext.cpp` — UBX + NMEA          |
| `int` | SIM7080G-GNSS im Modem     | `mod_modem.cpp` — AT-Kommandos          |
| `off` | kein GPS                   | —                                       |

Zu klären: Bleibt das so, oder legen wir uns auf das externe Modul fest?

Was für eine Festlegung spricht:

- Die beiden Pfade haben nichts gemeinsam — UBX-Binärprotokoll gegen AT-Kommandos,
  eigene Parser, eigene Zustandsmaschinen, eigene Fehlerbilder.
- Das Modem-GNSS ist L1-only und liefert alle 5 s; der DAN-F10N liefert 1 Hz auf
  L1+L5. Qualitativ ist das keine gleichwertige Alternative, sondern ein Rückfall.
- `cfg_mod_gps()` heißt „internes GPS", `mod_gps` klingt aber nach „GPS überhaupt".
  Diese Doppeldeutigkeit hat in dieser Sitzung bereits zu einer Fehldiagnose geführt
  (Logzeile „Modul deaktiviert" bei völlig gesundem externem GPS).

Was dagegen spricht:

- Das Modem-GNSS ist der Rückfall, wenn das externe Modul ausfällt oder fehlt.
- Die Lite-Variante hat kein Modem — dort ist `ext` ohnehin der einzige Weg.

Ein Mittelweg wäre, `int` als reinen Notnagel zu behalten, aber nicht mehr
gleichwertig zu pflegen, und die Flag-Benennung zu entwirren.

**Nicht verwechseln:** Der Branch `gps/ubx-ack-baud-autodetect` betrifft
ausschließlich das externe Modul. Er hat mit dem Modem-GNSS nichts zu tun.

### Ruhestrom des DAN-F10N ist nicht gemessen

Die README nannte früher 5–8 mA — der Wert stammt aus der M10-Zeit und gilt für
dieses Modul nicht. Ohne angeschlossenen Akku kann die AXP2101 keinen Strom
messen; für eine belastbare Zahl braucht es ein Messgerät in der DC5-Leitung.

## Erledigte Punkte

- WiFi Guard: komplett entfernt (2026-04-10)
- PLMN Block-System: Soft/Hard-Block entfernt, Green-List vereinfacht (2026-04-10)
- Kompass: mod_compass entfernt
- Traccar + InfluxDB: entfernt, ersetzt durch MQTT
- ig-Logik (Ignition): entfernt
- GPS Ext inject_assistnow: entfernt
- GPS: GLONASS-Konfiguration entfernt — auf F10 SPG existiert der Schlüssel nicht (2026-08-26)
- GPS: Antennenspannungs-Befehl entfernt — DAN-F10N führt die Supervisor-PIOs nicht heraus (2026-08-26)
