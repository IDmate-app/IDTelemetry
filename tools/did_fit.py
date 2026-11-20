#!/usr/bin/env python3
"""Find the range among the identifiers of a measurement series.

Input: did_runs.jsonl from the stick (Debug page -> Messreihe Reichweite).
Each line is one run: the range shown in the car ("km"), SoC, odometer,
temperatures and the raw answer of every identifier ("r": {"ECU:DID": "hex"}).

For every identifier and every way to read a number from its bytes (one byte,
two bytes big/little endian, signed or not, at each position) the script fits
km = a * value + b over all runs (least squares) and ranks the candidates by
the largest error. A value that only follows the SoC fits well, too; so the
script also fits the SoC alone and marks candidates that do not beat it.

Needs at least 3 runs with different ranges; more runs at different
temperatures separate range from SoC.

    python3 tools/did_fit.py did_runs.jsonl [--top 25]
"""
import argparse
import json
import math
import sys


def load(path):
    runs = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(d.get("km"), (int, float)) and d["km"] >= 0 and d.get("r"):
                runs.append(d)
    return runs


def readings(hexstr):
    """All numbers that can be read from the bytes: (name, value)."""
    try:
        b = bytes.fromhex(hexstr)
    except ValueError:
        return []
    out = []
    for i in range(len(b)):
        out.append((f"u8[{i}]", b[i]))
        if i + 1 < len(b):
            be = b[i] << 8 | b[i + 1]
            le = b[i + 1] << 8 | b[i]
            out.append((f"u16be[{i}]", be))
            out.append((f"u16le[{i}]", le))
            out.append((f"s16be[{i}]", be - 65536 if be >= 32768 else be))
    return out


def fit(xs, ys):
    """Least squares y = a*x + b -> (a, b, max abs error, r2) or None."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    b = my - a * mx
    err = [y - (a * x + b) for x, y in zip(xs, ys)]
    syy = sum((y - my) ** 2 for y in ys)
    r2 = 1 - sum(e * e for e in err) / syy if syy else 0.0
    return a, b, max(abs(e) for e in err), r2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    runs = load(args.file)
    kms = [r["km"] for r in runs]
    print(f"{len(runs)} Messläufe, Reichweiten: {sorted(set(kms))}")
    if len(runs) < 3 or len(set(kms)) < 3:
        print("Zu wenig: mindestens 3 Messläufe mit verschiedener Reichweite nötig.")
        return 1

    soc_runs = [r for r in runs if (r.get("soc") or -1) >= 0]
    soc_fit = fit([r["soc"] for r in soc_runs], [r["km"] for r in soc_runs]) if len(soc_runs) >= 3 else None
    if soc_fit:
        print(f"Nur aus dem SoC: km = {soc_fit[0]:.3f} * SoC + {soc_fit[1]:.1f}, "
              f"größter Fehler {soc_fit[2]:.1f} km, R² {soc_fit[3]:.4f}")

    keys = sorted({k for r in runs for k in r["r"]})
    cands = []
    for key in keys:
        present = [r for r in runs if key in r["r"]]
        if len(present) < 3 or len({r["km"] for r in present}) < 3:
            continue
        per_run = [dict(readings(r["r"][key])) for r in present]
        names = set(per_run[0]).intersection(*per_run[1:])
        for name in names:
            xs = [p[name] for p in per_run]
            ys = [r["km"] for r in present]
            f = fit(xs, ys)
            if not f:
                continue
            a, b, maxerr, r2 = f
            if r2 < 0.9 or a == 0:
                continue
            # a plausible scale: 1, 0.5, 0.25, 0.1, 2 … and a small offset rank higher
            nice = min(abs(a / s - 1) for s in (1, 0.5, 0.25, 0.1, 0.01, 2, 10))
            cands.append((maxerr, -r2, nice, key, name, a, b, len(present)))
    cands.sort()
    if not cands:
        print("Kein Kandidat mit R² ≥ 0,9.")
        return 0
    print(f"\n{'Kennung':<20} {'Lesart':<11} {'km = a·Wert + b':<26} {'max.Fehler':>10} {'R²':>8}  Läufe")
    for maxerr, nr2, nice, key, name, a, b, n in cands[:args.top]:
        flag = ""
        if soc_fit and maxerr >= soc_fit[2] * 0.8:
            flag = "  (nicht besser als SoC)"
        print(f"{key:<20} {name:<11} {f'{a:.4f}·x {b:+.1f}':<26} {maxerr:>10.1f} {-nr2:>8.4f}  {n}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
