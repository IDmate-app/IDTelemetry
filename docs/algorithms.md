# Algorithms

Pseudo-code descriptions for the trickier flows in the firmware.
For high-level structure see the [PlantUML diagrams](.) (`*.puml`).

---

## Telemetry store — raw partition ring buffer

`mod_telem_store.cpp`. Custom 5 MB flash partition, no filesystem.

### Layout

```
Slot         = 128 bytes (114 B payload + 1 B status + 13 B padding)
Erase block  = 4 KB  →  32 slots per block
Partition    = 5 MB  →  1280 blocks  →  40,960 slot capacity
```

### Slot status byte

The status byte uses single bit-flips so each transition is a one-byte
flash write without erasing the block. NAND flash can flip `1 → 0`
freely; only `0 → 1` needs a full block erase.

| Value | Meaning           | How reached                  |
| ----- | ----------------- | ---------------------------- |
| 0xFF  | empty             | freshly erased block         |
| 0xFE  | written, unsent   | bit 0 flipped (append)       |
| 0xFC  | sent / ack'd      | bit 1 flipped (publish ack)  |

### Append (crash-atomic)

```
function append(row):
    if pending == capacity:
        # Buffer full — sacrifice oldest block (32 rows lost, logged)
        erase_block(tail)
        tail = (tail + 32) mod capacity
        pending -= 32

    if head % 32 == 0:
        # Starting a fresh block — erase if it still holds old 0xFC slots
        if status[head] != 0xFF:
            erase_block(head)

    # Write payload bytes 1..127, status byte stays 0xFF
    flash_write(slot_offset(head) + 1, row.body, 127)

    # Commit: single-byte flip 0xFF → 0xFE
    flash_write(slot_offset(head), 0xFE, 1)   ← atomic point

    head = (head + 1) mod capacity
    pending += 1
```

A crash *before* the status flip leaves the slot at `0xFF` — the next
append simply overwrites the partial bytes. A crash *after* the flip
leaves a valid 0xFE row.

### Peek / Ack

```
function peek(out_row):
    if pending == 0: return false
    slot = flash_read(slot_offset(tail), 128)
    if slot.status != 0xFE: return false   # defensive
    out_row = slot.body
    return true

function ack():
    # Commit: single-byte flip 0xFE → 0xFC
    flash_write(slot_offset(tail), 0xFC, 1)
    tail = (tail + 1) mod capacity
    pending -= 1
```

A crash between peek and ack means the row stays `0xFE` and gets
re-sent on the next boot — at most a duplicate, never a loss.

### Boot scan

```
function boot_scan():
    statuses[0..capacity] = flash_read_all_status_bytes()

    written_count = count_where(statuses == 0xFE)
    if written_count == 0:
        head = tail = pending = 0
        return  # empty

    # tail = first 0xFE preceded by a non-0xFE
    tail = find first i where statuses[i] == 0xFE
                       and statuses[i-1 mod cap] != 0xFE
    # head = first non-0xFE walking forward from tail
    head = find first i (starting at tail) where statuses[i] != 0xFE
    pending = written_count
```

Boot scan reads the partition once (1280 × 4 KB ≈ <1 s on QSPI). The
cost is paid only once at boot.

---

## MQTT watchdog — 4-stage escalation

`mod_modem.cpp`, around the modem task's `STATE_RUNNING` block.

The escalation is sticky between attempts: each failure level increments
its own counter, and only successful publish resets them all.

```
loop every 10s:
    if mqtt_publish_succeeded:
        mqtt_fail_count = 0
        modem_reset_count = 0
        plmn_scan_count = 0
        continue

    mqtt_fail_count += 1

    # Stage 1 — reconnect MQTT only
    if mqtt_fail_count < MQTT_FAIL_RESET_COUNT:    # 3
        try mqtt_reconnect()
        continue

    # Stage 2 — modem hardware reset
    if modem_reset_count < MODEM_RESET_ESCALATE:   # 2
        AT+CNACT=0,0       # release PDP context
        modem_pwrkey_pulse()
        wait_for_at_ok(5s)
        modem_reset_count += 1
        mqtt_fail_count = 0
        continue

    # Stage 3 — PLMN scan (provider switch)
    if plmn_scan_count < PLMN_SCAN_ESCALATE:       # 1
        candidates = AT+COPS=?
        filter candidates against PLMN_TABLE whitelist
        try register on each candidate in priority order
        plmn_scan_count += 1
        if success: save_last_plmn()
        mqtt_fail_count = 0
        modem_reset_count = 0
        continue

    # Stage 4 — full reboot, but capped
    if wd_reboot_count >= WD_REBOOT_MAX:           # 3 / 30 min
        # Cap reached — keep buffering in raw partition, no more reboots
        plmn_scan_count = 0
        modem_reset_count = 0
        mqtt_fail_count = 0
        continue

    spiffs_lock(2000)        # graceful: no mid-write rename
    spiffs_unlock()
    wd_reboot_count += 1     # RTC_DATA_ATTR — survives esp_restart
    esp_restart()
```

The reboot counter (`wd_reboot_count`) lives in `RTC_DATA_ATTR` memory
and survives `esp_restart`. Once it hits the limit (3 reboots / 30 min),
the device stops trying and just buffers rows into the raw partition
indefinitely — preserving up to ~5,000 km of telemetry until coverage
returns.

---

## GPS row capture — distance / curve / time triggers

`row_try_capture()` in `mod_telemetry.cpp`. Runs every 3 s in TELEM task
and is also invoked synchronously from the MODEM task via
`telem_force_capture()` — hence the dedicated `s_capture_mtx`.

```
function row_try_capture():
    lock(s_capture_mtx, timeout=100ms)            # serialise vs MODEM task
    if not gps_valid:
        yaw_peak = 0
        unlock; return

    dist     = haversine_m(last_cap, current_pos)
    elapsed  = millis() - last_cap_ms

    # Glitch filter — reject impossible position jumps
    max_jump = max(500, min(2000, elapsed/1000 * 200))   # 200 m/s cap
    if dist > max_jump:
        log("GPS-Glitch rejected")
        unlock; return

    # Speed-aware distance threshold
    dist_thresh = match speed:
        >110 km/h → 350 m
        > 80 km/h → 200 m
        > 50 km/h → 150 m
        else      → 100 m

    # Triggers, evaluated in priority order
    if speed >= MIN_SPEED and yaw_peak >= TURN_DPS and curve_cooldown_expired:
        reason = "curve"
    elif dist >= dist_thresh:
        reason = "distance"
    elif elapsed >= MAX_INTERVAL and speed >= MIN_SPEED:
        reason = "time"
    else:
        yaw_peak = 0; unlock; return  # no capture

    row = snapshot_telemetry_cache()              # under s_mutex
    row.gps = current_pos
    telem_store_append(row)                       # crash-safe persist
    last_cap = current_pos
    last_cap_ms = now
    unlock
```

---

## Sleep / wake

`mod_sleep.cpp`. See [`sleep_wake.puml`](sleep_wake.puml) for the state
chart. Key invariant: deep sleep only when both motion AND VBUS are absent.

```
function sleep_update():    # called every loop tick
    if VBUS present:
        last_vbus_ms = millis()
        if sleep_request_pending:
            cancel_pending_sleep()
        return

    vbus_gone = millis() - last_vbus_ms
    if vbus_gone < SLEEP_NO_VBUS_MS (5 min):
        return                                      # grace period

    if last_motion_ms > 0 and (millis() - last_motion_ms) < INACTIVITY:
        return                                      # still moving

    # Soft request: let modem flush pending rows, then suspend
    request_sleep()
    wait up to 2 min for tasks to drain
    enter_deep_sleep()

function enter_deep_sleep():
    set g_shutdown                                  # tasks exit cleanly
    wait up to 2 s for tasks to release I2C/CAN/UART
    telem_persist_to_spiffs()                       # cache + row queue
    modem_poweroff()
    gps_ext_idle()                                  # M10 keeps tracking
    configure wake sources:
        EXT0 = MPU-6050 INT (motion)    GPIO3 high
        EXT1 = AXP2101 INT (VBUS)       GPIO6 low
    esp_deep_sleep_start()
```
