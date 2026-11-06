# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/),
versioning per [SemVer](https://semver.org/).

## [1.6.5] – 2026-11-06

### Fixed

- **Ring buffer started at slot 0 after every boot with nothing unsent** ("Boot-Scan: leer"): each new trip overwrote the already sent rows of the last trips from the front, which "send again" still needs. Now the boot scan continues behind the newest row (highest timestamp), so sent rows stay until the buffer really wraps.
- **False "battery critical" sleep**: 25 s after a web update, with flickering VBUS, the stick went to sleep with "Akku kritisch" although the battery read 100 % an hour before. The low reading must now last 30 s (and resets whenever VBUS is seen).

## [1.6.4] – 2026-11-06

### Fixed

- **Long silence after losing the network on the road**: after a drop the modem reported "not registered, not searching" (`RegStatus=0`) for 13 minutes while the network search only waited (60 s, 5 min, then 15 min each); only a restart of the stick brought the network back, within a second. Now:
  - the modem reports "not searching" for 2 minutes → radio off and on (`CFUN=0/1`, settings re-applied; no PWRKEY pulse);
  - with OBD power (car on or charging) the search waits at most 60 s between attempts; without power the long pauses stay to save the battery;
  - 10 minutes without network with OBD power → orderly restart of the stick, at most twice in a row (a real dead spot, e.g. an underground car park, does not cause a reboot loop). The counter resets once the network is back or the stick goes to sleep.

## [1.6.3] – 2026-11-06

### Added

- **Send again from a point in time** (configuration page, "Daten erneut senden"): rows already sent in a chosen period go out once more with their original timestamps — e.g. after the server could not read them because of a wrong key. Sent slots cannot be flipped back without an erase, so they are appended again as new rows; the server just overwrites what it already has. At most a quarter of the buffer per request. Runs in the telemetry task; the result shows on the page and in the syslog.

### Fixed

- **Release firmware without a key sent unreadable telegrams**: release builds only carry the public example key (all zeros). With an empty AES field the stick encrypted with it, the broker confirmed every row, and the server dropped them all — silently. Now the stick sends nothing with the example key, keeps the rows in the buffer, writes a warning to the syslog every 5 minutes and shows a red notice on the configuration page.
- **An empty or invalid AES field no longer deletes a stored key.**
- **A new AES key applies right after saving**, without a restart (before: only after the next boot).

## [1.6.2] – 2026-11-06

### Added

- **Setting "OBD power is switched"** (configuration page, CAN bus & vehicle alarm). On: power at the stick means the car is awake, so it reads without the movement condition (e.g. while charging). Off (default): permanent power at the OBD port — reading only with power and movement, as before.
- **Listen-only without power**: when VBUS is gone the CAN controller switches to `LISTEN_ONLY` (no requests, no ACK bits). If the bus still carries traffic, a statistic goes to the syslog every minute (`CAN passiv: … Rahmen/min, … Kennungen`) — ROADMAP 6, test 2, without a laptop. Back to normal as soon as power returns.

### Changed

- **Falling asleep after power loss**: stays awake while something moves (getting out, plugging in the charger, getting back in). Sleeps once power has been gone ≥ 3 min and nothing moved for 2 min; at the latest after 15 min (carrying the stick must not drain its battery). Before: fixed 5 min.

## [1.6.1] – 2026-11-06

### Fixed

- **No readings while charging**: the stick talked to the car only with VBUS and a shake in the last 30 s (VW-alarm guard). At a charger nothing moves, so it went quiet although the car was awake. Now it keeps reading while the car itself reports "charging" (0x7448, at most 90 s old) and the bus answered in the last 30 s. When charging ends or the bus goes quiet, the old rule applies again at once, so the stick does not keep the car awake. The charging state (0x7448) is now polled every 15 s instead of 60 s, so a charge is caught within the 30 s after the last movement.

## [1.6.0] – 2026-11-06

**One-time USB flash needed** (new partition table). After that, updates go through the stick's configuration page.

### Added

- **Firmware update over the web page**: two firmware slots (`app0`/`app1`). The upload goes into the free slot; the running one stays as the way back. The AP password is required, the file is checked before writing (ESP32-S3 image, full/lite by file name) and verified after (image + SHA). The release asset `idtelemetry-full-<version>.bin` is the file to upload.
- **Crash protection**: if a freshly uploaded firmware crashes three times in a row (panic/watchdog) before running 2 minutes, the stick boots the previous one again. Deep-sleep wakes and normal restarts do not count.
- **Back to the previous firmware**: button on the configuration page (shows its build ID).
- The web flasher writes `boot_app0` (otadata) as well, so a USB flash always starts the flashed firmware, even after a web update.

### Changed

- Partition table (full): `app1` 2 MB added, `telem` 5 → 3 MB (~24,500 rows), `coredump` moved. `nvs` (configuration), `app0` and `spiffs` (logs) stay where they are. The `telem` partition is erased once on the first start with the new layout — send the backlog before flashing (normally it is empty).
- Lite: SPIFFS moves and is formatted once (logs gone).

## [1.5.2] – 2026-11-06

### Added

- **DID 0x2AB8 (ECU 710)**: ABRP asks for it every 5 s and every request went onto the bus. The stick now polls it itself every 10 s and answers ABRP from its cache. The value falls steadily while driving (likely the BMS energy content).
- **Self-calibration**: the stick integrates its own power (voltage × current, every 5 s) between two 2AB8 readings ≥ 100 units apart and logs Wh per unit (`CAL2AB8` in the syslog, `cal_2ab8` in `/status`, serial `rawdid`). Nothing is sent to IDmate until the factor is stable.

### Changed

- The configuration page hides the web update when the partition table has no second firmware slot (this stick: one slot → USB only) instead of failing on upload.

## [1.5.1] – 2026-11-06

### Fixed

- **Log download** streams the file in 2 KB pieces straight from the log instead of copying it first: copying a 730 KB log blocked the web server for seconds (watchdog risk). Pages and log previews are sent with `Cache-Control: no-store`, so a browser never shows a page from before the flash.
- Configuration page named the old GPS module (BLITZ Mini M10); it is the u-blox DAN-F10N (L1+L5) since 1.4.0. The compass switch (BLITZ M10 board only, never had a driver) is hidden.

## [1.5.0] – 2026-11-06

Replaces the mixed state found on the author's stick (FW_VERSION "1.2.0" + August GPS work, without the 1.3.0/1.4.0 fixes); everything in that image is contained here. The SoE / HVAC / cabin-temperature formulas are still open: until they are filled in, those fields are polled and logged raw but not sent.

### Added

- **Build ID**: `git describe` is compiled in as `FW_BUILD` (`scripts/build_id.py`). Shown in the web UI header, `/status` (`fw_build`, `fw_variant`), the boot syslog and the serial command `version`. `-dirty` = built from uncommitted code.
- **Info telegram** `0x10` once per MQTT connection (LTE and WiFi): `{"fw","build","var","pv","bits"}`. IDmate 1.2.255+ shows the stick firmware; older servers drop it.
- **New fields** SoE (bit 20, kWh), HVAC power (bit 21, kW), cabin temperature (bit 22, °C), appended to the v2 value telegram (version byte stays `0x02`; older servers ignore the tail).
- The stick polls `0x0364` (ECU FC0076) and `0x2613` (ECU 746) itself every 30 s. Formulas are still open: raw bytes go to the syslog (`RAWDID`, every 5 min), `/status` (`raw_dids`) and the serial command `rawdid`. ABRP is answered from these raw bytes while they are fresh.
- `/sys-log-tail` and `/elm-log-tail` (`?n=` bytes, max 16 KB) for the debug page preview.

### Fixed

- **Logs vanished**: `/download-sys-log` and `/download-elm-log` renamed the log, so every call (and the debug page preview every 15–20 s) emptied it. The download now copies; the preview reads only the end of the file.
- The debug page says when the ELM log is switched off instead of hiding the section.
- **No upload for 20–35 min after every connection drop** (seen ~10× in the logs): MQTT was configured only once, a dead PDP context was never renewed, and the PWRKEY "reset" switched the running modem off; only the escalation reboot helped. Now SMDISC + SMCONF on every connect, the PDP context is dropped after a failed MQTT connect, and the first escalation rebuilds the radio like a boot (register → PDP → MQTT) without PWRKEY or reboot.
- **Start without network → no upload until reboot** (ROADMAP 5): one failed SIM check while the modem was busy searching switched LTE off for good ("GPS-only"). Now 3 tries, and the SIM is re-checked every 60 s. During the network backoff (up to 15 min) the stick now looks every 15 s whether the modem has found an allowed network on its own. If the modem stays silent after the PWRKEY reset, a second pulse switches it back on.

### Changed

- Ring-buffer slot layout: valid flags as a bitmask (room for 29 fields). Slots written by 1.4.x and earlier are still read, so an update keeps the unsent backlog. The RAM row file across deep sleep is discarded once on the update.

## [1.4.0] – 2026-10-28

### Added

- **u-blox DAN-F10N** as external GPS, **dual-band L1+L5**. The configuration is checked on every start, so L5 stays on after the module lost power in sleep.
- **Faster cold start**: the last known position is kept in NVS and handed to the module as a starting point.
- **Serial diagnostics**: `pmu`, `gps standby`, `gps raw`, `gps gsv`, `gps cfg <group>`, `gps powercycle <s>`, `can selftest`, `can listen <s>` (listen-only, sends nothing).

### Fixed

- **CAN stayed dead after a bus-off** until the next reboot — the driver is now restarted after recovery and retried.
- **CAN buffer overflow**: long UDS answers (e.g. the 17-byte VIN) could overrun the caller's buffer.
- **Frozen GPS position**: when the module goes silent, the last position no longer counts as valid after 10 s.
- **Phantom satellites**: GSV signal IDs (L5/E5a/B2a) were counted as satellites.
- **Modem**: a modem that never answers no longer blocks boot and sleep; PDP setup is time-limited; MQTT state is read exactly instead of matching any `1`.
- **Battery**: detection is switched on in the AXP2101, and "not connected" is told apart from "level not known yet".
- **RTC**: impossible DS1307 values (month 25) are rejected instead of corrupting timestamps; missing date is logged instead of silently dropping rows.

### Changed

- GPS cold-start baud rate 38400 (DAN-F10N default) — about 1.2 s faster.
- The 20 s settle time also applies after a warm start: a valid 3D fix can still be kilometres off.

## [1.3.0] – 2026-06-13

### Fixed

- **External GPS**: UBX configuration is checked against the module's ACK, the baud rate is detected automatically, and satellites in view are counted once per unique SV.
- **Boot hang and sleep panic** eliminated — no more stall during boot or panic on the sleep path (B.2, B.3).
- **Telemetry store**: appends skip non-deleted slots, and a layout marker guards the slot format (B.1, B.5).
- **WiFi upload**: MQTT drain runs as QoS 1 via espMqttClient — no silent row loss on a flaky link (B.4).
- **GPS snapshot consistency**, BATT row value and a SPIFFS lock leak fixed (B.6, B.7, B.8).
- **Sleep**: undervoltage protection added; the PMU-fault reboot loop can no longer occur (B.9, B.10).
- **RTC**: DS1307 I²C glitches are plausibility-checked and a time jump must be confirmed before it is accepted (B.11).
- **Compass setting** (`mod_compass`) is persisted across reboots (B.17).
- Assorted race and correctness fixes (B.17, P3 hygiene).

### Changed

- **Modem network search** uses backoff and throttles `AT+COPS=?` scans to once per hour (B.12).

### Security

- **Web/config hardening**: secret handling, XSS and input validation tightened (B.13–B.16).

## [1.2.0] – 2026-05-14

### Added

- **5 MB raw partition for telemetry rows** (`mod_telem_store`): dedicated flash partition, no filesystem. 40,960 slots of 128 B each, atomic commit via status byte (`0xFF`→`0xFE`→`0xFC`). Drop-in replacement for the old SPIFFS queue. ~5,000 km of offline buffering, cannot be destroyed by SPIFFS issues.
- **Web UI embedded into the app binary** (`board_build.embed_txtfiles`): HTML/CSS/JS now live in the app partition, not in SPIFFS. No more `uploadfs` required for UI updates, no more race conditions with log writers.
- **Passive signal monitoring** via `AT+CPSI?`: logged every 60 s to syslog with RSRP/RSRQ/RSSI/SNR/band/PLMN. New serial command `lte sig` for on-demand query. Non-destructive — replaces guesswork with data.
- **SPIFFS mutex** (`spiffs_lock`/`spiffs_unlock`): recursive mutex serialises ALL SPIFFS access. Previously `syslog()`, `log_add()`, `telem_persist` ran concurrently from 8+ tasks without synchronisation → heap corruption → PANIC in the FreeRTOS scheduler.
- **`row_try_capture` serialised** (`s_capture_mtx`): prevents the TELEM-task / MODEM-task race on `s_cap_lat/lon/ms` (which surfaced as '9999m' distance triggers in the logs).

### Changed

- **Partition layout** grown from ~4 MB usable to the full 16 MB. app0 2 MB, telem 5 MB new, spiffs 8.8 MB (was 2.18 MB).
- **AP timeout** now only applies when VBUS is gone. `softAPdisconnect()` while driving was an ipc0 crash trigger.
- **Modem boot throttle**: no row-publish burst during the first 15 s after boot. Prevents ipc0 spinlock timeouts during parallel module init.
- **Graceful pre-`esp_restart()` in MQTT escalation**: acquires `spiffs_lock` before the restart, prevents mid-write corruption of the queue file.
- **`sys.log` cap** 1024 KB → 768 KB (irrelevant with the new large SPIFFS partition, kept defensively).
- **GPS_EXT stack** 3 KB → 4 KB defensively.

### Removed

- **SPIFFS telemetry queue** (`/telem_q.dat`): replaced by the raw partition. Code remains in place but is no longer invoked.

## [1.1.0] – 2026-04-26

### Added

- **PLMN scan + whitelist** (`AT+COPS=?`) as an escalation step when auto mode finds no registrable network. Telekom DE (26201) is preferred at boot with a 15 s timeout, then fallback to last successful provider, then auto, then scan.
- **Numeric PLMN in MQTT payload** (bit 19, u16) — enables server-side provider statistics without operator strings.
- **Coredump partition** (64 KB) — after boot, PANIC backtrace, task and PC are written to syslog and the image is erased. New `coredump` partition in `partitions.csv`.
- **GPS glitch filter**: position jumps >200 m/s (720 km/h) are discarded. Capped at max 2 km between two rows.
- **GPS time sanity check**: UBX frames with year outside 2024–2040 are ignored (protects against cold-start glitches with year 2093/2165).
- **PMU 0% glitch filter**: if the AXP2101 coulomb counter reports 0% but cell voltage >3.3 V, the previous value is retained.
- **BLE diagnostics**: connect/disconnect with peer MAC, connection interval, supervision timeout, uptime and flap marker (<500 ms).
- **AES-256-CBC encryption** for MQTT payload (pre-shared key in `secrets.h`).
- **SPIFFS row queue** — telemetry is crash-safely buffered and re-sent row-by-row (oldest first) after connection drops up to multi-hour outages.

### Changed

- **PMU charger**: constant current 300 mA → **1000 mA**, VBUS input limit explicitly to **2000 mA** (default was 500 mA and was actually limiting charge current).
- **PMU update interval**: battery level is now read fresh via I2C every 10 s (previously only on web hits — cache stayed stale since boot).
- **MQTT watchdog, 4-stage**: (1) reconnect every 10 s → (2) modem reset (PWRKEY) after 3 fails → (3) PLMN scan after 2 modem resets → (4) reboot with RTC counter (max. 3 in 30 min).
- **Reboot limit**: `RTC_DATA_ATTR` counter survives `esp_restart`. Prevents endless reboot loops in permanent dead zones.
- **GPRS IP validation**: a successful `getLocalIP()` returning `0.0.0.0` is rejected, connection re-established.
- **SoC sample logic**: `min_delta` 0.3 → 0.1, `max_age` 90 s → 300 s — fills gaps in the server dashboard without flooding.
- **VBUS debouncing**: 3× read with inverse edge detection prevents PMU glitches from being interpreted as fake sleep triggers.
- **Binary MQTT protocol**: bitmask + fields, AES-256-CBC. Replaces direct HTTP/Influx push.
- **AP timeout**: 10 min → 2 min without a client.

### Removed

- **WiFi Guard** dropped — VBUS detection is sufficient as a safety lock.
- **Green-list PLMN**: replaced by the scan whitelist.
- **Ignition logic (`ig=`)**: VBUS trailing time is not reproducible, adds no value.
- **InfluxDB HTTP + Traccar HTTP**: replaced by persistent MQTT push.
- **Compass module**: redundant against GPS heading.

### Fixed

- **eq_mask bug**: SoC plateaus on the server were caused by unchanged fields being incorrectly marked `valid=false`.
- **Recovery after GPRS fail**: premature state transition into `STATE_WAIT_FOR_NETWORK` → SIM re-init → "GPS-only mode" — removed, escalation now does its job naturally.
- **uint32_t underflow** in the gyro sleep check (race condition).
- **Crash in `send_html`** on missing SPIFFS file.
- **Curve trigger** takes priority over distance trigger (otherwise late captures in sharp turns).
- **Rows on publish failure**: no longer discarded, flow cleanly into the SPIFFS queue.
- **MQTT reconnect spam** during permanent connection loss.
- **AT+SMSSL** no longer sent on plain MQTT (SIM7080G FW R1951.07).

## [1.0.6] – 2026-03 (earlier)

- HTML cleanup, shared CSS/JS, README rewrite, i18n fixes.
- For earlier development see `git log`.

[1.4.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.4.0
[1.3.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.3.0
[1.2.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.2.0
[1.1.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.1.0
