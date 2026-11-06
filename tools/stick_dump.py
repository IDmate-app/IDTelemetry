#!/usr/bin/env python3
"""Flash-Abbild des Sticks offline zerlegen (ROADMAP 0, Firmware-Abgleich).

    esptool.py --chip esp32s3 read_flash 0 0x1000000 stick_full_JJJJ-MM-TT.bin
    python3 tools/stick_dump.py stick_full_JJJJ-MM-TT.bin [--out DIR] [--repo .]

Liest die Partitionstabelle aus dem Abbild und schreibt nach DIR (Standard:
<abbild>_teile/):

  app0.bin        die Firmware (nur dieser Teil darf weitergegeben werden)
  app_info.txt    ESP-IDF-Beschreibung: Kompilierdatum/-zeit, IDF, ELF-SHA
  strings.txt     alle Texte der Firmware (Log-Meldungen, Versionen)
  spiffs/         sys.log, elm.log, ... (mit mkspiffs, sonst Textzeilen
                  herausgeschnitten → spiffs_carved_sys.log)
  telem.txt       Ringpuffer: Slots je Status und Layout, ungesendete Zeilen

Mit --repo: welcher Commit passt am besten zu den Texten der Firmware.

NVS (AES-Key, APN, WLAN) wird NICHT gelesen und nicht ausgegeben. Das
Abbild selbst enthält es trotzdem: privat aufbewahren, nicht hochladen.
"""

import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter
from datetime import datetime, timezone

PART_TABLE_OFF = 0x8000
APP_DESC_MAGIC = 0xABCD5432


def partitions(img):
    out = []
    for i in range(0, 0xC00, 32):
        e = img[PART_TABLE_OFF + i:PART_TABLE_OFF + i + 32]
        if len(e) < 32 or e[:2] != b'\xaa\x50':
            break
        ptype, sub, off, size = e[2], e[3], *struct.unpack_from('<II', e, 4)
        name = e[12:28].split(b'\x00')[0].decode('ascii', 'replace')
        out.append({'name': name, 'type': ptype, 'sub': sub, 'off': off, 'size': size})
    return out


def app_desc(app):
    """esp_app_desc_t steht direkt hinter Image- und erstem Segment-Header."""
    off = 24 + 8
    if len(app) < off + 256 or struct.unpack_from('<I', app, off)[0] != APP_DESC_MAGIC:
        return None
    s = lambda a, n: app[off + a:off + a + n].split(b'\x00')[0].decode('ascii', 'replace')
    return {'version': s(16, 32), 'project': s(48, 32), 'time': s(80, 16), 'date': s(96, 16),
            'idf': s(112, 32), 'elf_sha256': app[off + 144:off + 176].hex()}


def image_len(app):
    """Länge des App-Images aus den Segment-Headern (+ Prüfsumme, + SHA)."""
    if len(app) < 24 or app[0] != 0xE9:
        return len(app.rstrip(b'\xff'))
    nseg, pos = app[1], 24
    for _ in range(nseg):
        if pos + 8 > len(app):
            return len(app.rstrip(b'\xff'))
        pos += 8 + struct.unpack_from('<I', app, pos + 4)[0]
    pos += 1                          # Prüfsummen-Byte
    pos = (pos + 15) & ~15            # auf 16 aufgefüllt
    if app[23] == 1:                  # hash_appended
        pos += 32
    return pos


def strings(data, n=6):
    return [m.group().decode('ascii') for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, data)]


def find_mkspiffs():
    pats = [os.path.expanduser('~/.platformio/packages/tool-mkspiffs/mkspiffs*'),
            os.path.expanduser('~/.platformio/packages/tool-mkspiffs/*/mkspiffs*')]
    for p in pats:
        for f in sorted(glob.glob(p)):
            if os.access(f, os.X_OK):
                return f
    return shutil.which('mkspiffs')


def spiffs_extract(blob, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    tool = find_mkspiffs()
    raw = os.path.join(out_dir, '..', 'spiffs.bin')
    with open(raw, 'wb') as f:
        f.write(blob)
    if tool:
        r = subprocess.run([tool, '-u', out_dir, '-b', '4096', '-p', '256', '-s', str(len(blob)), raw],
                           capture_output=True, text=True)
        files = os.listdir(out_dir)
        if r.returncode == 0 and files:
            return f'mkspiffs: {len(files)} Dateien → {out_dir}: ' + ', '.join(sorted(files))
        msg = f'mkspiffs fehlgeschlagen ({r.returncode}): {r.stderr.strip()[:200]}'
    else:
        msg = 'mkspiffs nicht gefunden (PlatformIO: pio pkg install -g -t platformio/tool-mkspiffs)'
    # Notlösung: Syslog-Zeilen "[Zeit]\tKAT\tText" aus den Seiten schneiden.
    # SPIFFS-Seiten = 256 B mit 5 B Kopf; Zeilen über Seitengrenzen gehen verloren.
    lines = set()
    for m in re.finditer(rb'\[[0-9: .\-]{5,19}\]\t[A-Z_]{2,8} *\t[\x20-\x7e\xc0-\xff\x80-\xbf]{1,120}\n', blob):
        lines.add(m.group().decode('utf-8', 'replace'))
    carved = os.path.join(out_dir, '..', 'spiffs_carved_sys.log')
    with open(carved, 'w') as f:
        f.writelines(sorted(lines))
    return f'{msg}\nNotlösung: {len(lines)} Syslog-Zeilen herausgeschnitten → {carved} (Reihenfolge nach Zeit sortiert, unvollständig)'


def telem_summary(blob):
    status = Counter()
    layout = Counter()
    newest = oldest_pending = None
    for off in range(0, len(blob) - 127, 128):
        st = blob[off]
        status[st] += 1
        if st in (0xFE, 0xFC):
            layout[blob[off + 1]] += 1
            ts = struct.unpack_from('<I', blob, off + 4)[0]
            if 1_600_000_000 < ts < 2_200_000_000:
                newest = ts if newest is None else max(newest, ts)
                if st == 0xFE:
                    oldest_pending = ts if oldest_pending is None else min(oldest_pending, ts)
    t = lambda ts: datetime.fromtimestamp(ts, timezone.utc).strftime('%Y-%m-%d %H:%M UTC') if ts else '-'
    name = {0xFF: 'leer', 0xFE: 'ungesendet', 0xFC: 'gesendet'}
    out = [f'Slots: {sum(status.values())}']
    out += [f'  {name.get(k, hex(k))}: {v}' for k, v in sorted(status.items())]
    out.append('Layout-Marker (reserved[0]) beschriebener Slots: ' +
               ', '.join(f'{k:#04x}×{v}' for k, v in sorted(layout.items())))
    out.append('  0xff = unversioniert (vor B.5), 0x16 = 22 Felder (1.4.x), 0x99 = 1.5.0 (25 Felder)')
    out.append(f'Neueste Zeile: {t(newest)} · älteste ungesendete: {t(oldest_pending)}')
    return '\n'.join(out)


_ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', '0': '\0'}


def _c_strings(text):
    """String-Literale aus C/C++-Quelltext (Kommentare, Zeichen-Literale und
    #include-Zeilen übersprungen)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('//', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif c == '/' and text.startswith('/*', i):
            i = text.find('*/', i + 2)
            i = n if i < 0 else i + 2
        elif c == '#' and text[text.rfind('\n', 0, i) + 1:i].strip() == '' and text.startswith('#include', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == '\\' else 1
            i = j + 1
        elif c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"' and text[j] != '\n':
                if text[j] == '\\' and j + 1 < n:
                    buf.append(_ESC.get(text[j + 1], ' '))
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            out.append(''.join(buf))
            i = j + 1
        else:
            i += 1
    return out


def literals(text):
    """Text-Literale des Quellcodes als druckbare ASCII-Stücke (≥ 8 Zeichen):
    Escapes, Umlaute und printf-Platzhalter zerteilen den Text im Binary."""
    out = set()
    for m in _c_strings(text):
        m = re.sub(r'%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h|z)?[diouxXfeEgGcspu%]', '\n', m)
        pieces = tuple(sorted(x for x in re.findall(r'[\x20-\x7e]{8,}', m)))
        if pieces:
            out.add(pieces)
    return out


def match_commits(repo, img_strings):
    """Symmetrischer Vergleich: Texte des Commits, die im Image fehlen, plus
    Projekt-Texte im Image, die der Commit nicht hat. Kleinste Summe = Stand."""
    joined = '\n'.join(img_strings)
    git = lambda *a: subprocess.run(['git', '-C', repo, *a], capture_output=True, text=True,
                                    errors='replace').stdout
    blob_cache, per_rev = {}, {}
    for rev in git('rev-list', '--all').split():
        lits = set()
        for line in git('ls-tree', '-r', rev, 'src').splitlines():
            meta, _, path = line.partition('\t')
            if not path.endswith(('.cpp', '.h')):
                continue
            sha = meta.split()[2]
            if sha not in blob_cache:
                blob_cache[sha] = literals(git('cat-file', '-p', sha))
            lits |= blob_cache[sha]
        if lits:
            per_rev[rev] = lits
    found = {lit: all(x in joined for x in lit) for lits in per_rev.values() for lit in lits}
    in_image = {lit for lit, ok in found.items() if ok}
    rows = []
    for rev, lits in per_rev.items():
        missing = sum(1 for lit in lits if not found[lit])
        extra = len(in_image - lits)
        rows.append((missing + extra, missing, extra, rev))
    rows.sort()
    out = []
    for total, missing, extra, rev in rows[:8]:
        info = git('log', '-1', '--format=%h %ad %s', '--date=short', rev).strip()
        out.append(f'Abweichung {total:4d} (fehlt im Image {missing:3d}, nur im Image {extra:3d})  {info}')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('--out')
    ap.add_argument('--repo', help='IDTelemetry-Checkout für den Commit-Vergleich')
    a = ap.parse_args()
    img = open(a.dump, 'rb').read()
    out = a.out or os.path.splitext(a.dump)[0] + '_teile'
    os.makedirs(out, exist_ok=True)
    print(f'Abbild: {len(img):#x} Bytes, SHA-256 {hashlib.sha256(img).hexdigest()[:16]}…')

    parts = partitions(img)
    if not parts:
        sys.exit('Keine Partitionstabelle bei 0x8000 gefunden.')
    for p in parts:
        print(f"  {p['name']:<9} typ {p['type']:#04x}/{p['sub']:#04x}  {p['off']:#09x}  {p['size']:#09x}")
    by = {p['name']: p for p in parts}

    app_p = by.get('app0') or next((p for p in parts if p['type'] == 0), None)
    if app_p:
        app = img[app_p['off']:app_p['off'] + app_p['size']]
        app = app[:image_len(app)]
        with open(os.path.join(out, 'app0.bin'), 'wb') as f:
            f.write(app)
        d = app_desc(app)
        st = strings(app)
        with open(os.path.join(out, 'strings.txt'), 'w') as f:
            f.write('\n'.join(st))
        info = [f'app0: {len(app)} Bytes, SHA-256 {hashlib.sha256(app).hexdigest()}']
        if d:
            info += [f'{k}: {v}' for k, v in d.items()]
            info.append('  (Arduino: Datum/Zeit/Version stammen aus der vorgebauten Bibliothek,'
                        ' nicht vom Stick-Build — für den Abgleich nicht brauchbar)')
        vers = sorted({s for s in st if re.fullmatch(r'\d+\.\d+\.\d+(-[\w.\-]+)?', s)})
        info.append('Versions-Texte: ' + ', '.join(vers[:20]))
        info.append('Build-Kennung: ' + ', '.join(s for s in st if re.fullmatch(r'\d+\.\d+\.\d+-\d+-g[0-9a-f]+(-dirty)?', s)) or '-')
        for k in ('Telemetry Stick v', 'Baudrate erkannt', 'L1+L5', 'L5', 'Info gesendet', 'RAWDID',
                  'Netz im Backoff', 'Snapshot', 'fw_version', 'fw_build'):
            info.append(f'  {"ja " if any(k in s for s in st) else "nein"}  "{k}"')
        txt = '\n'.join(info)
        open(os.path.join(out, 'app_info.txt'), 'w').write(txt + '\n')
        print(txt)

    if 'spiffs' in by:
        p = by['spiffs']
        print(spiffs_extract(img[p['off']:p['off'] + p['size']], os.path.join(out, 'spiffs')))

    if 'telem' in by:
        p = by['telem']
        txt = telem_summary(img[p['off']:p['off'] + p['size']])
        open(os.path.join(out, 'telem.txt'), 'w').write(txt + '\n')
        print(txt)

    if a.repo and app_p:
        print('Passende Commits (Text-Vergleich; 0 = gleiche Texte, Rest = Code hinter Feature-Schaltern):')
        for line in match_commits(a.repo, st):
            print('  ' + line)
    print(f'Fertig → {out}/  (NVS nicht gelesen; Abbild privat halten)')


if __name__ == '__main__':
    main()
