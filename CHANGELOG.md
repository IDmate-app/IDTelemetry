# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/),
versioning per [SemVer](https://semver.org/).

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

[1.3.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.3.0
[1.2.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.2.0
[1.1.0]: https://github.com/IDmate-app/IDTelemetry/releases/tag/v1.1.0
