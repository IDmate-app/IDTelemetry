# IDTelemetry

> 🧪 **Beta.** The firmware runs daily in the author's cars, but it is a hobby project: expect rough edges, read the changelog before updating, and keep a USB cable at hand for re-flashing.

> 📊 **Companion server:** [IDmate](https://github.com/IDmate-app/IDMate) — the dashboard & trip log that consumes the telemetry and lets you analyse trips, charges and battery health.

**Open telemetry for the VW ID family** — an ESP32-based stick that turns the OBD2 port into a live data feed for SoC, battery temperature, power flow, charging behavior and location.

Born out of frustration: the VW ID.7 shows you almost nothing about what's happening under the hood. No real SoC, no battery temperature, no power draw, no charge curve — not even a simple energy consumption history. Even a base-model VW Hybrid puts more data on the dashboard than an ID.7 does. This project is a DIY box that plugs into OBD2 and turns a black-box EV into something you can actually understand — your data, your broker, your dashboard.

## What It Does

- Reads battery, drivetrain and climate data from the CAN bus (UDS/ISO-TP)
- Pushes telemetry to **InfluxDB** over LTE-M for Grafana dashboards
- Buffers **~40,000 rows (~5,000 km)** in a dedicated 5 MB Flash partition — survives reboots, crashes and long radio dead zones
- Tracks location via **Traccar** (OsmAnd protocol)
- Emulates a **BLE ELM327** so ABRP sees the car natively
- Runs a **WiFi AP with a full Web-UI** for live data, config and debugging
- Sleeps in the **low-µA range** when parked, wakes on motion

## Architecture

![System Architecture](docs/architecture.png)

The stick reads CAN data via UDS, persists every telemetry row to a dedicated 5 MB raw Flash partition (~40,000 rows ≈ 5,000 km offline buffer), pushes encrypted telemetry over LTE-M MQTT, exposes a BLE ELM327 emulation for ABRP, and serves a local Web-UI on its WiFi AP.

- High-level structure: [`docs/*.puml`](docs/) — rendered with `plantuml`, themed via [`docs/_theme.puml`](docs/_theme.puml)
- Step-by-step algorithms (telemetry store, MQTT escalation, sleep/wake, GPS capture): [`docs/algorithms.md`](docs/algorithms.md)

### Storage Layout

The 16 MB Flash is split into:

| Partition | Size | Purpose |
| --------- | ---- | ------- |
| app0 | 2 MB | Firmware (Web-UI HTMLs are embedded inside) |
| coredump | 64 KB | Crash dumps for post-mortem |
| telem | 5 MB | Raw ring buffer for telemetry rows (no filesystem — atomic per-slot status byte) |
| spiffs | 8.8 MB | Runtime logs only |

HTMLs live inside the firmware binary, not in SPIFFS — they cannot be corrupted by log writes or filesystem issues. The telemetry ring buffer is crash-safe by design: each row is committed via a single status-byte flip, so a power loss mid-write loses at most one row.

> ⚠️ **No OTA — USB flashing only.** The partition table has an `otadata` entry but only a single app slot (`app0`/`ota_0`, no `ota_1`, no `factory`). OTA updates are therefore impossible despite the misleading `otadata` presence — every firmware update is a USB re-flash. Any future partition-layout change forces a full re-flash with an erase of the `telem` partition, which loses the unsent backlog. Plan flash windows accordingly (keep the previous build as a rollback `.bin`).

<!-- -->

> ⚠️ **No time source ⇒ no telemetry.** Rows are dropped at capture when `rtc_unix_ms() == 0` — i.e. an empty DS1307 cell *and* no GPS *and* no LTE time means the device silently records nothing. There is no `settimeofday`/`configTime` fallback. This is intentional (it prevents 1970-timestamped garbage from reaching the receiver), but means a dead RTC battery combined with no fix produces a silent telemetry gap rather than an error.

### Modem Watchdog

When MQTT or GPRS misbehave, the firmware escalates through four levels before giving up — built to survive long radio dead zones without endless reboot loops.

![Modem Escalation](docs/modem_escalation.png)

### Sleep / Wake

![Sleep / Wake](docs/sleep_wake.png)

## Hardware

> ⚠️ **Board requirements**
>
> The firmware is built specifically for ESP32-S3 with **16 MB flash and 8 MB PSRAM** (the `N16R8` chip variant). Smaller flash or no-PSRAM boards will not boot — the partition table assumes a 16 MB layout, and the embedded Web UI plus the 5 MB raw telemetry partition do not fit in less. Plain ESP32 (non-S3) boards are not supported.

| Component | Details |
| --------- | ------- |
| MCU | LILYGO T-SIM7080G-S3 (ESP32-S3, 16 MB Flash, 8 MB PSRAM) |
| CAN Transceiver | SN65HVD230 (3.3 V, active mode) |
| Modem | SIM7080G (LTE-M / NB-IoT), onboard |
| GPS | SparkFun u-blox DAN-F10N (L1+L5 dual-band GNSS, integrated patch antenna, ML414H backup cell) — external via UART |
| GPS (fallback) | SIM7080G integrated GNSS (used when external GPS is disabled) |
| PMU | AXP2101 (onboard, manages DC rails for modem, GPS, peripherals) |
| RTC | DS1307 + CR2032 (keeps time across power cycles) |
| IMU | MPU-6050 (accelerometer + gyroscope, motion wake-up) |

## GPS Modes

The firmware supports two GPS configurations controlled by `GPS_EXT_ENABLED` in `config.h`:

**External GPS (default, recommended):**
The DAN-F10N runs continuously on UART2 (GPIO1/GPIO2) and maintains a fix independently of the modem. LTE stays connected via MQTT at all times — no GPS/LTE cycling needed. It keeps tracking across sleep transitions; power comes from the PMU DC5 rail (3.3 V).

The module reports `MOD=DAN-F10N`, `FWVER=SPGL1L5 6.00`, `PROTVER=40.00` and serves GPS, Galileo, BeiDou, SBAS, QZSS and NavIC. **GLONASS is not supported** — the F10 is an L1+L5 receiver and GLONASS L1OF broadcasts FDMA around 1602 MHz, outside the L1 filter. The firmware enables GPS + Galileo + BeiDou + SBAS (EGNOS); QZSS and NavIC are regional systems over East Asia and India and stay off at European latitudes.

GPS L5 needs one extra step. L5 is still pre-operational, so the satellites broadcast themselves as unhealthy and the receiver drops them from the navigation solution. The firmware sets the undocumented health-override key `0x10320001`, measured on the device via `UBX-NAV-SIG`: GPS L5 goes from 0 to 5 satellites in the solution, total signals used 32 → 37.

Field experience so far: in bad weather the dual-band receiver has been the only module to achieve a fix at all — under the same conditions neither the earlier L1-only module nor the SIM7080G's internal GNSS got one. Its ML414H backup cell keeps the receiver's RTC and ephemerides alive while the DC5 rail is switched off in sleep, which in practice delivers the first fix roughly 3 minutes earlier.

> Idle current has not been measured on this module. The figure previously quoted here (~5–8 mA) came from the earlier M10 hardware and does not apply.

**Internal GPS (fallback):**
When `GPS_EXT_ENABLED` is set to `false`, the SIM7080G's built-in GNSS is used instead. Because the SIM7080G shares a single radio frontend, GPS and LTE data cannot run simultaneously. The device cycles between modes every 60 seconds:

```text
GPS active (55 s) → GPS off → LTE on → send data → LTE off → GPS on
```

The external GPS eliminates this limitation entirely.

## Web UI

The device opens a WiFi Access Point:

- SSID: `IDTelemetry`
- Password: `IDTelemetry1`
- URL: `http://192.168.4.1`

Three pages:

- **Data** — live telemetry table
- **Debug** — gyro graph, compass, CAN tools, syslog viewer
- **Config** — AP credentials, SIM / APN (full variant only), MQTT broker + AES key, **WiFi Upload (STA)** SSID / password / endpoint URL, BLE behaviour, SPIFFS logging toggles, OTA firmware update

The Config page is the single source of runtime configuration — everything that used to live in `secrets.h` can be edited here and is persisted in NVS.

> ⚠️ **WiFi Upload slots: use `http://` or `mqtt://` only.** `https://` endpoints are **not verified to work** — the upload path calls `HTTPClient::begin(url)` without a `WiFiClientSecure`/CA, so depending on the Arduino core `begin()` may silently fail and rows would never upload (the breakage only shows up at drain time). Until a `https://` slot has been verified on a test ESP against a real endpoint, configure upload URLs as plain `http://` or `mqtt://`.

## Installation Guide

The setup has two main paths:

- **End-user path** (Web Flasher + Config page) — needs only a USB cable and a phone or laptop. No source code, no toolchain. See [Step 1](#step-1--flash-the-firmware) below.
- **Developer path** (clone + PlatformIO) — for building from source or modifying. Jump to [Building from source](#building-from-source).

### Bill of Materials

| Variant | Part | Description | approx. Price |
| ------- | ---- | ----------- | ------------- |
| both | OBD2 plug + cable | 16-pin OBD2 connector (pin 6 + 14 for CAN, pin 16 for 12 V) | ~5 € |
| both | SN65HVD230 CAN transceiver | 3.3 V CAN bus module | ~3 € |
| both | DS1307 RTC module | Real-time clock (I2C) with coin cell | ~2 € |
| both | MPU-6050 breakout | Accelerometer / gyroscope (I2C) | ~3 € |
| both | USB-C cable | For flashing and power | — |
| full | LILYGO T-SIM7080G-S3 | ESP32-S3 board with LTE-M/NB-IoT modem, AXP2101 PMU, Li-Po | ~35 € |
| full | SparkFun u-blox DAN-F10N | L1+L5 dual-band GNSS with integrated antenna (UART) — recommended | |
| full | LTE-M SIM card | e.g. ThingsMobile (prepaid, no contract) | ~15 € |
| lite | ESP32-S3-DevKitC-1 (N16R8) | Plain ESP32-S3 board, 16 MB flash, 8 MB PSRAM | ~12 € |

### Step 1 — Flash the firmware

> 💡 The easiest path is the **Web Flasher** — no toolchain, no IDE. If that's not available for your release yet, see [Building from source](#building-from-source) further down.

Open the [Web Flasher](https://idmate-app.github.io/IDTelemetry/flasher/) in Chrome or Edge, plug the stick in via USB, click **Install** on the matching variant (`Full` for LILYGO T-SIM7080G-S3, `Lite` for plain ESP32-S3).

### Step 2 — Connect to the stick AP

Right after flashing the stick spans up its own WiFi:

- SSID: `IDTelemetry`
- Password: `IDTelemetry1`

iOS / Android pop up a captive-portal sheet with a **Setup** button — click it. Otherwise open <http://192.168.4.1> in any browser.

### Step 3 — Configure backends in the Web UI

Everything you used to put into `secrets.h` lives in the Config page now, persisted in NVS. Fill in whatever applies to your setup:

| Section | Fields | When to fill |
| ------- | ------ | ------------ |
| **WiFi Upload (STA)** | Home WiFi SSID + password, Upload URL | Always if you want a WiFi upload path (mandatory for the lite variant, optional secondary for full) |
| **MQTT Broker** | Host, port, topic, AES-256 key | Full variant — primary upload path over LTE-M |
| **MQTT Broker → AES-256 Key** | 64 hex chars | Overrides the compile-time key from `secrets.h`. Leave blank to keep the built-in default. |
| **SIM / APN** | PIN, APN, user, pass | Full variant only |
| **WiFi Access Point** | AP SSID + password | Optional — only if you want the stick to advertise a different AP than the default |

Click **Save**. APN and AP credentials require a restart to take effect; the rest is live.

### Step 4 — Verify

- **Data page** shows live telemetry once CAN is up and the car is awake.
- **Debug page** shows the syslog stream — look for `MQTT Verbunden`, `STA verbunden`, or `LTE: RSRP=…` lines to confirm each upload path.
- ABRP / Torque can pair to BLE `OBDII`.

---

## Building from source

If you want to modify the firmware or run a development build, follow this section instead of Step 1.

### Add credentials

```bash
cp src/secrets.h.example src/secrets.h
```

`secrets.h` provides the *compile-time defaults* for everything that can be overridden via the Web UI. You only have to fill it in if you intend to flash a pre-configured stick (a single fleet of sticks that all use the same MQTT broker, AES key, etc.). For one-off setups, leave the defaults and configure everything via Step 3 above.

> ⚠️ **Generating tokens, keys and secrets**
>
> Use a cryptographically secure random number generator. **Never** ask a chatbot or LLM to "give me a random 32-byte key" — LLM output is probability-distributed, not random. The same prompt produces a small, predictable distribution of values that an attacker can enumerate offline. The convenience is not worth the silent loss of entropy.
>
> ```bash
> openssl rand -hex 32                                  # AES-256 key
> openssl rand -base64 32                               # API/webhook tokens
> python3 -c "import secrets; print(secrets.token_hex(32))"
> ```
>
> The MQTT AES key in `secrets.h` **must match** the `MQTT_AES_KEY` configured on the [IDmate](https://github.com/IDmate-app/IDMate) server side.

### Wiring

```text
LILYGO T-SIM7080G-S3          External Modules
┌──────────────────┐
│                  │         ┌─── CAN Bus (OBD2) ───┐
│ GPIO17 (CAN TX) ─┼──→ SN65HVD230 CTX ──→ OBD2 Pin 6  (CAN-H)
│ GPIO18 (CAN RX) ─┼──→ SN65HVD230 CRX ──→ OBD2 Pin 14 (CAN-L)
│                  │
│                  │         ┌─── I2C Bus (shared) ──┐
│ GPIO45 (SDA) ────┼──→ DS1307 + MPU-6050 + QMC5883L (SDA)
│ GPIO21 (SCL) ────┼──→ DS1307 + MPU-6050 + QMC5883L (SCL)
│                  │
│ GPIO3  (INT) ────┼──← MPU-6050 INT (deep sleep wake-up)
│                  │
│                  │         ┌─── External GPS ──────┐
│ GPIO1  (RX) ─────┼──← DAN-F10N TX  (UART2, 38400 baud — auto-detected)
│ GPIO2  (TX) ─────┼──→ DAN-F10N RX
│                  │
│ 3V3 ─────────────┼──→ VCC for CAN transceiver + RTC
│ GND ─────────────┼──→ GND for all modules
└──────────────────┘

Onboard (no wiring needed):
  GPIO5/4   → SIM7080G UART   (modem, UART1)
  GPIO41    → SIM7080G PWRKEY
  GPIO42    → SIM7080G DTR
  GPIO40    → SIM7080G STATUS
  GPIO44    → SIM7080G FLIGHT (RF kill)
  GPIO15/7  → AXP2101 I2C     (PMU, Wire1)
  GPIO6     → AXP2101 INT     (VBUS wake-up)
```

Insert a nano-SIM card into the slot on the bottom of the LILYGO board.

**Note:** The MPU-6050 AD0 pin must be pulled to 3.3 V so it uses I2C address `0x69` (avoiding collision with the DS1307 at `0x68`). The DAN-F10N is powered by the PMU DC5 rail (3.3 V).

### Build and flash from PlatformIO

```bash
# Install toolchain
# - VSCode + PlatformIO IDE extension, OR
# - just `pio` from `pip install platformio`

# Build + upload the full variant (LILYGO T-SIM7080G-S3)
pio run -e s3_full --target upload
pio run -e s3_full --target uploadfs    # only on first flash, initialises SPIFFS

# Or the lite variant (plain ESP32-S3)
pio run -e s3_lite --target upload
pio run -e s3_lite --target uploadfs    # only on first flash
```

Pre-built binaries from the [Releases page](https://github.com/IDmate-app/IDTelemetry/releases) can also be flashed directly with `esptool.py` — see [`docs/flasher/README.md`](docs/flasher/README.md) for the byte-offset table that ESP Web Tools uses.

## GPIO Reference

| GPIO | Function | Direction | Bus |
| ---- | -------- | --------- | --- |
| 1 | External GPS RX (← F10 TX) | IN | UART2 |
| 2 | External GPS TX (→ F10 RX) | OUT | UART2 |
| 3 | MPU-6050 INT (deep sleep wake) | IN | EXT1 |
| 4 | Modem RX (← SIM7080G TX) | IN | UART1 |
| 5 | Modem TX (→ SIM7080G RX) | OUT | UART1 |
| 6 | PMU interrupt (VBUS wake) | IN | — |
| 7 | PMU SCL | BIDIR | I2C Wire1 |
| 15 | PMU SDA | BIDIR | I2C Wire1 |
| 17 | CAN TX | OUT | TWAI |
| 18 | CAN RX | IN | TWAI |
| 21 | Sensor SCL | BIDIR | I2C Wire0 |
| 40 | Modem STATUS | IN | — |
| 41 | Modem PWRKEY | OUT | — |
| 42 | Modem DTR | OUT | — |
| 44 | Modem FLIGHT / RF kill | OUT | — |
| 45 | Sensor SDA | BIDIR | I2C Wire0 |

I2C Wire0 devices: DS1307 (`0x68`), MPU-6050 (`0x69`). The GNSS module is on UART2, not I2C.

## Build Variants

Two PlatformIO environments share the same source tree:

| Env | Hardware | Includes | Status |
| --- | -------- | -------- | ------ |
| `s3_full` (default) | LILYGO T-SIM7080G-S3 | LTE-M modem, external GNSS, AXP2101 PMU, Li-Po, BLE, CAN, raw 5 MB telemetry buffer | ✅ production |
| `s3_lite` | Plain ESP32-S3 (N16R8) with 16 MB flash | BLE ELM327, CAN, raw 1 MB telemetry buffer, WiFi-only upload | 🧪 scaffolding only — not yet runnable |

Build with `pio run -e s3_full` or `pio run -e s3_lite`. The active env selects partition table and `FEATURE_*` compile flags. See [`platformio.ini`](platformio.ini).

### Lite variant — caveats

The lite variant is positioned as a hobby-grade option for users who already have a plain ESP32-S3 (N16R8) board and want BLE ELM327 + CAN + WiFi upload without buying the LILYGO board with modem and PMU. It is **not feature-complete yet** — feature flags compile cleanly, but the WiFi upload path and `#ifdef` gating are still being written.

> ⚠️ **The lite variant requires an external GNSS module.** The plain ESP32-S3 board has no built-in GNSS, and the lite build does not include the SIM7080G internal GNSS path. Wire a u-blox DAN-F10N (or any NMEA-output GNSS module) to UART2 (GPIO1 RX / GPIO2 TX); the firmware auto-detects 38400 / 115200 / 9600 baud. Without it, the firmware boots but no location data is captured.

---

> ⚠️ **Power supply for the lite variant is unresolved.**
>
> The lite variant is intended to be powered **via USB only** — i.e. always-on power from the OBD2 power pin or a 12 V → 5 V USB converter. Do **not** wire it to the car's permanent +12 V tap.
>
> When the vehicle goes to sleep (CAN bus quiet), any node still writing on CAN will trigger the wake-up alarm and eventually flag a CAN diagnostic fault. The lite variant has no AXP2101 / battery / sleep flow yet — so it must be on switched power that turns off with the ignition (OBD2 pin 16 is permanent on most VWs, but the cigarette-lighter USB ports usually switch with ignition).
>
> The `s3_full` variant solves this via VBUS detection + gyro motion wake — that whole layer is missing in lite.

## Safety & Power Management

### WiFi Guard

CAN bus TX is **only enabled while the configured car WiFi SSID is visible**. If the car's hotspot goes out of range, TX is locked immediately. This prevents accidental bus writes when the stick is powered outside the vehicle. An alternative guard mode uses VBUS detection (external 12 V power present = car is on).

### Deep Sleep

When the gyro detects no movement (car parked):

1. Final telemetry batch is sent
2. GPS enters idle mode (module keeps tracking, resumes instantly)
3. ESP32-S3 enters deep sleep (µA range)
4. Wakes on MPU-6050 motion interrupt (GPIO3) or VBUS insertion (GPIO6)

## Open-Source Dependencies

| Library | License | Author |
| ------- | ------- | ------ |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | Apache 2.0 | Espressif |
| [AsyncTCP](https://github.com/mathieucarbou/AsyncTCP) | LGPL v3 | mathieucarbou |
| [ESPAsyncWebServer](https://github.com/mathieucarbou/ESPAsyncWebServer) | LGPL v3 | mathieucarbou |
| [TinyGSM](https://github.com/vshymanskyy/TinyGSM) | LGPL v3 | vshymanskyy |
| [ArduinoJson](https://arduinojson.org) | MIT | Benoit Blanchon |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Apache 2.0 | h2zero |
| [ArduinoHttpClient](https://github.com/arduino-libraries/ArduinoHttpClient) | Apache 2.0 | Arduino |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | MIT | lewis he |
| Space Mono & Syne | OFL | Google Fonts |

**Notes:**

- ELM327 AT command set: proprietary protocol by [ELM Electronics](https://www.elmelectronics.com). This project implements an emulation layer for ABRP compatibility.
- OBD-II PIDs: SAE J1979 / ISO 15031 standard, freely usable.
- VW-specific UDS DIDs (service 0x22): community reverse-engineering, no official documentation.

## Development

This project follows the **HITL principle (Human in the Loop)**: concept, decisions and direction come from the human — code is AI-assisted.

## License

GNU Affero General Public License v3.0 © 2026 Ingo F — see [LICENSE](LICENSE)

---

[![Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/lordvonbaum)

> Donations are voluntary and solely support the project. They have no influence on bug prioritization, feature requests or support.
