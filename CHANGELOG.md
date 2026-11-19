# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/),
versioning per [SemVer](https://semver.org/).

## [1.7.9] – 2026-11-19

### Added

- **What the car answers per identifier**: once per start, for every identifier the stick polls, the syslog (`CAN`) shows the answer bytes, the rejection code (NRC) or "keine Antwort" after three tries. The range (`0x0295`) never arrived in IDmate; this shows whether the car answers it at all.
- **DID search** on the Debug page: reads every identifier of a range on a chosen control unit (read only, only while the car is awake) and lists the answers. With the range shown in the car entered, answers that contain the value (one or two bytes, also scaled ×10, ÷2, ÷4, ±3) are marked as hits; repeating after the range changed narrows it down. Needs the login from 1.7.8.

## [1.7.8] – 2026-11-19

### Added

- **Login for the web UI**: with an AP password set, `/api/config` no longer shows passwords, keys and upload URLs without login, and saving the settings, sending on the CAN bus (`/send`, `/scan`, `/monitor`, `/restart-can`, `/api/canpid`) and opening the pairing window answer 401. Log in on the Config page with the AP password (`/api/login`, session cookie, 30 min sliding). Wrong passwords are slowed down and logged. Without AP password everything stays open as before.
- **Way back in**: holding the boot button for 5 s while the stick runs opens the web UI without password for 3 minutes (syslog `WEB`), e.g. to set a new AP password.

## [1.7.7] – 2026-11-19

### Fixed

- **Same firmware uploaded again reported as failed**: installing the running firmware once more switched the slot as it should, but the next start said "Update NICHT uebernommen" and the page showed a red cross, because only the build string was compared. The stick now also remembers the slot before the update: same build in the other slot is reported as "Gleiche Firmware erneut aufgespielt", in green on the page.

### Changed

- **Build ID starts with the own version**: `<version>-g<commit>` (e.g. `1.7.7-gabc1234`, `-dirty` for uncommitted code) instead of `git describe`. The release tag is created only after the build, so 1.7.6 was called "1.7.5-1-gb5594b5".

## [1.7.6] – 2026-11-19

### Added

- **Charging curve**: while the car is charging, the stick now also writes rows when standing still – every 15 s on a fast charger (DC), every minute on AC. Up to now rows only came from driving (distance, curve, or every minute above 3 km/h), so a charge with someone in the car showed up in IDmate as just two or three rows although the stick was reading all the time. Without a GPS fix (garage, hall) a charging row takes the last row's position.

## [1.7.5] – 2026-11-19

### Changed

- **"Parked" from the gear bit** (DID `0x210E`): byte B carries the gear. Seen on the car: `08` = P, `07` = R, `05` = D, and `0C` = P with an extra bit after switching off. Parked is now B & 0x08 instead of the exact value 8, so `0C` counts as parked as well. New values still go to the syslog once per start, now with the result.

### Added

- **Which task hangs on sleep**: when tasks do not stop in time before deep sleep, the syslog warning now names them ("Task-Timeout — erzwinge Stop (…)").

## [1.7.4] – 2026-11-17

### Fixed

- **Shared I2C bus without a lock**: the RTC (DS1307) and the motion sensor (MPU-6050) share one bus. The bus driver only locks a single transfer, not the read-out afterwards, so the gyro task (every 100 ms) and the web server reading the clock could get each other's bytes – wrong motion values (which also gate CAN sending) or a wrong time. A common lock now keeps write, request and read-out together. The PMU is on its own bus and not affected.

### Added

- **Observation for "parked"** (DID `0x210E`): parked is still recognised by the exact value B = 8. Each value the car sends is written to the syslog once per start (`CAN`, "0x210E neu"), so the rule can be settled on real data later.

## [1.7.3] – 2026-11-17

### Fixed

- **Web update failing silently**: when the upload broke off (connection lost, restart during the upload), the page still showed a green "restarting"; rejected uploads (wrong password, variant, image) and write or check errors only went to the serial console. Now every failure goes to the syslog (`OTA`, with the reason and the bytes received), an upload that did not finish is reported at the next start, and the next start also tells whether the update took: "Update uebernommen: old -> new" or "Update NICHT uebernommen". The settings page remembers the running build before the upload, reloads after the restart and compares: it says whether the new firmware runs, and shows the result of the last update under the firmware slot. A new upload after a broken one no longer fails at the start.
- **CAN driver restarted on every voltage dropout**: while driving, the PMU now and then reported the supply as gone for 1–2 s, and each time the CAN driver was reinstalled for listen-only mode and back (175 times on one trip). Listen-only mode now starts only after the supply has been gone for 3 s; back to normal operation is immediate. Sending stays blocked during any dropout as before.

## [1.7.2] – 2026-11-14

### Added

- **Switch for the mobile network** (*Config → SIM / APN*, on by default): off, the modem stays unpowered – no LTE, no data use, no network search. The stick then works like the Lite variant: it keeps collecting and hands the data over only in a WLAN from *WiFi Upload*; on the road the server does not see the car. Bluetooth (ABRP helper, live data) is unaffected, so the three data paths – server over LTE, ABRP over Bluetooth, display device over Bluetooth – can each be switched on their own. The modem's internal GNSS is off with it; the external GPS is not. Values learned by IDmate arrive over WLAN only. The status display shows `LTE OFF`. Takes effect after a restart.

### Changed

- **End of a trip without the modem task** (mobile network off): "supply voltage gone for 5 s → take the last point and stop recording" and the point "Schlafen" before sleeping were both done by the modem task. With the modem off the sleep logic does both itself, so a trip ends the same way with and without LTE. Nothing changes while the modem is on.

## [1.7.1] – 2026-11-14

### Fixed

- **Two Bluetooth devices at once** (ABRP on the phone and a display device on the tablet): the stick stopped advertising with the first connection, so a second device could not find it; and when one device disconnected, the ELM327 emulation took the other one for gone too and ABRP got no more answers. The stick now keeps advertising while a connection is free (3 in total), and "disconnected" means that no device is connected any more. A further device connecting no longer discards ABRP's command in progress.

### Added

- **Switch for the ABRP helper** (*Config → BLE / ABRP*, on by default): off, the stick does not offer the ELM327 emulation (service `FFE0`). With both the helper and the live data off, the stick does not advertise over Bluetooth at all. Takes effect after a restart.

## [1.7.0] – 2026-11-14

### Added

- **Live data over Bluetooth for display devices** ([docs/live-ble.md](docs/live-ble.md)): a paired device in the car – e.g. the IDmate app for Android Auto – gets state of charge, power, speed, odometer, temperatures, charging state and position straight from the stick, as CBOR telegrams over a GATT notification (1 s while driving, 30 s standing). Off by default (*Config → BLE / ABRP*), takes effect after a restart. The stick stays the one OBD master: the feature reads the telemetry cache and does not query the bus. The ELM327 emulation for ABRP is unchanged.
- **Pairing window**: the telegrams go to a device paired with a six-digit code only (link encrypted and authenticated). The code is created when **Pair a device** is pressed on the config page, is valid for 3 minutes and for one attempt, and is not stored. A fixed code could be worked out from radio range in about 20 attempts; outside the window a secret random code is in force that is replaced after every attempt.

## [1.6.11] – 2026-11-07

### Added

- **Which task ran when the watchdog fired**: on a task-watchdog reset the stick now notes the task running on each core (RTC memory, survives the reset) and writes it to the syslog after the restart, e.g. `WATCH Task-Watchdog nach 512s Laufzeit: Kern 0 lief "ipc0", Kern 1 "async_tcp"`. Diagnosis only, no change in behaviour (step 1 of the watch tower). Trigger: the crash of 2026-11-06 15:04 during a charging stop, where only the watchdog itself was visible in the backtrace.

## [1.6.10] – 2026-11-07

### Changed

- **Dashboard odometer everywhere**: the km offset from IDmate (fixed, the dashboard reads ~5 km more than CAN) is now added to the rows sent to IDmate too, not only to the answers to ABRP. The info telegram reports the offset in force (`"odo"`), so IDmate knows from when its km are dashboard km (the charge print stops adding its own +5 km from then on; needs IDmate 1.2.269). The switch on the configuration page now only covers the display SoC for ABRP.

## [1.6.9] – 2026-11-07

### Fixed

- **Interior temperature 10 °C too high**: the OBDb formula (A:B / 10) gave 31–32 °C with the climate set to 20 °C; ABRP shows the sensor 1–2 °C above the setting. Now A:B / 10 − 10 °C (log of 2026-11-06: 20.9–22.3 °C).

## [1.6.8] – 2026-11-07

### Added

- **ABRP gets the SoC and odometer as shown in the car.** IDmate learns both with "BMS lernen" (display SoC as a function of the CAN SoC, optionally with temperatures; odometer offset from the dashboard km typed in once) and sends them to the stick, retained on `<topic>/cfg`, encrypted with the same AES key as the telegrams. The stick applies them only to its Bluetooth answers to ABRP (DID 028C SoC, 295A odometer); what it sends to IDmate stays raw, the learning needs it. Received over LTE and WiFi, stored in NVS, shown on the configuration page (BLE / ABRP), switchable there ("ABRP bekommt SoC und km-Stand wie im Auto", default on). Needs IDmate 1.2.267.

## [1.6.7] – 2026-11-06

### Added

- **Energy content, auxiliary power and cabin temperature are now decoded and sent** (payload bits 20–22, already understood by IDmate). Formulas from the open vehicle database OBDb (Volkswagen-ID3) and checked against this car's raw bytes:
  - `2AB8` (ECU 710): bytes C:D × 25 Wh = energy content (kWh). Check: 54.5 kWh at SoC 65.9 % and 51.75 kWh at 62.4 %, both ≈ 83 kWh at 100 %.
  - `0364` (ECU FC0076): signed A:B / 10 = power of all HV auxiliary consumers (heater, A/C, 12 V converter), kW. Log: 0.4–1.6 kW.
  - `2613` (ECU 746): signed A:B / 10 = interior temperature, °C.

## [1.6.6] – 2026-11-06

### Fixed

- **The last row of every trip was dropped**: a forced capture (power gone, going to sleep, charging events) got the placeholders 9999 m / 99999 ms, and the 2 km cap of the GPS glitch filter always rejected it ("GPS-Glitch verworfen: 9999m in 99999ms"). The parking position, final odometer and final SoC never reached the server. The filter now uses the real distance and time since the last row.
- **Send again could lose rows** when its sources lay right ahead of the write position (the copies erase block by block there). It now refuses such a period instead ("Zeitraum reicht bis an die aeltesten Zeilen").

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
