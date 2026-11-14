# Live data over Bluetooth

A display device in the car – for example the IDmate app for Android Auto – can read the vehicle state
straight from the stick, without the detour over LTE and the server. The stick stays the one OBD master:
this feature does not query the bus, it reads the telemetry cache the stick fills anyway.

**Off by default.** Switch it on in the Web-UI: *Config → BLE / ABRP → Live data over Bluetooth for display
devices*, save, restart the stick. While it is off the service does not exist.

## Pairing

The telegrams contain position and driving state, so they go to a **paired** device only.

1. On the config page press **Pair a device**. The stick creates a six-digit code and shows it for
   **3 minutes**. The page is only reachable over the stick's password-protected WiFi.
2. On the device, connect to the stick; Android asks for the code.
3. The link must be encrypted *and* authenticated. A device that paired without the code ("Just Works")
   gets nothing and is disconnected after 60 s.

**One code, one attempt.** A code that stays the same could be worked out by someone in radio range in
about 20 pairing attempts (the procedure gives away one bit per failed attempt). So the code exists only
while the pairing window is open, and every attempt – successful or not – uses it up; for another device,
press the button again. Outside the window a secret random code is in force that is shown nowhere and
replaced after every attempt. Nothing is stored: after a restart there is no code until the button is
pressed.

Devices already paired reconnect without a code; they keep working until their bond is removed on the
device.

The ELM327 emulation for ABRP (service `FFE0`) is unchanged and stays open as before.

## GATT

| | UUID | Properties |
|---|---|---|
| Service | `1d7e0001-6d61-7465-2d6c-697665000000` | |
| Telegram | `1d7e0002-6d61-7465-2d6c-697665000000` | Notify, Read (encrypted + authenticated) |

One notification = one telegram. Request an MTU of at least 185 – the largest telegram is 156 bytes; a
telegram that does not fit the negotiated MTU is not sent (counter `skipped`).

## Telegrams

The same telegram IDmate serves as JSON over its read API, encoded as CBOR (RFC 8949):

```json
{"v": 1, "src": "stick", "ts": "2026-11-12T14:03:21Z", "seq": 4711, "type": "status",
 "data": {"soc": 71.5, "range": 312, "power": -12.4, "speed": 87, "odometer": 23456.7,
          "charging": false, "dc": false, "t_ext": 9, "t_batt": 21}}
```

| `type` | `data` | When |
|---|---|---|
| `caps` | `{"status": {field: interval_s, …}, "position": {"lat": s, "lon": s}}` | once after subscribing |
| `status` | the fields above | every 1 s while driving, every 30 s when standing |
| `position` | `lat`, `lon` (WGS84, float64) | same rhythm, only with a GPS fix |

- A field without a fresh value (older than 2 min; odometer 15 min) is left out – never `null`, never `0`.
- `seq` counts up per stick start across all telegram types, so a client sees gaps.
- Without a time source the stick sends nothing (counter `no_time`), as it stores no rows either.

`/status` in the Web-UI carries `"live": {"on", "subs", "sent", "skipped", "no_time", "pair_s"}` (`pair_s`: seconds the
pairing window stays open, 0 = closed). `POST /live-pair` opens the window and returns the code.

## Test

`python3 tools/live_cbor_test.py` builds the encoder on the PC, decodes its output with an independent
CBOR reader and checks content and size.
