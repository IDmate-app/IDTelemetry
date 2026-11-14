#!/usr/bin/env python3
"""Checks src/live_cbor.h on the PC: builds tools/live_cbor_test.cpp, decodes its
output with an independent CBOR reader and compares the telegrams.
Run from the repository root:  python3 tools/live_cbor_test.py"""
import os
import struct
import subprocess
import sys
import tempfile

MTU_PAYLOAD = 185 - 3      # NimBLEDevice::setMTU(185): a notification carries MTU - 3 bytes


def decode(b, i=0):
    ib = b[i]; major, info = ib >> 5, ib & 31; i += 1
    if major in (0, 3, 5):
        if info < 24: n = info
        elif info == 24: n = b[i]; i += 1
        elif info == 25: n = struct.unpack(">H", b[i:i + 2])[0]; i += 2
        elif info == 26: n = struct.unpack(">I", b[i:i + 4])[0]; i += 4
        else: raise ValueError("length")
        if major == 0: return n, i
        if major == 3: return b[i:i + n].decode(), i + n
        out = {}
        for _ in range(n):
            k, i = decode(b, i); out[k], i = decode(b, i)
        return out, i
    if ib == 0xF4: return False, i
    if ib == 0xF5: return True, i
    if ib == 0xFA: return struct.unpack(">f", b[i:i + 4])[0], i + 4
    if ib == 0xFB: return struct.unpack(">d", b[i:i + 8])[0], i + 8
    raise ValueError("type 0x%02x" % ib)


def main():
    exe = os.path.join(tempfile.mkdtemp(), "live_cbor_test")
    subprocess.check_call(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", "src",
                           "tools/live_cbor_test.cpp", "-o", exe])
    got = {}
    for line in subprocess.check_output([exe], text=True).splitlines():
        p = line.split()
        if len(p) == 3:
            raw = bytes.fromhex(p[2]); assert len(raw) == int(p[1])
            val, end = decode(raw); assert end == len(raw), "trailing bytes in " + p[0]
            assert len(raw) <= MTU_PAYLOAD, "%s: %d bytes do not fit a notification" % (p[0], len(raw))
            got[p[0]] = (val, len(raw))
        else:
            got[p[0]] = int(p[1])
    ok = lambda a, b: abs(a - b) < 1e-3
    s, n = got["status"]
    assert (s["v"], s["src"], s["ts"], s["seq"], s["type"]) == (1, "stick", "2026-11-12T14:03:21Z", 4711, "status")
    d = s["data"]
    assert list(d) == ["soc", "range", "power", "speed", "odometer", "charging", "dc", "t_ext", "t_batt"]
    assert ok(d["soc"], 71.5) and ok(d["power"], -12.4) and ok(d["odometer"], 23456.7) and d["charging"] is False
    p, pn = got["position"]
    assert p["type"] == "position" and abs(p["data"]["lat"] - 50.123456) < 1e-9 and abs(p["data"]["lon"] - 12.654321) < 1e-9
    c, cn = got["caps"]
    assert c["type"] == "caps" and set(c["data"]["status"]) == set(d) and c["data"]["position"] == {"lat": 1, "lon": 1}
    assert got["few"][0]["data"] == {"soc": 20.0}
    assert got["none"] == 0 and got["nopos"] == 0 and got["small"] == 0
    print("ok: status %d B, position %d B, caps %d B (limit %d B)" % (n, pn, cn, MTU_PAYLOAD))


if __name__ == "__main__":
    sys.exit(main())
