#!/usr/bin/env python3
"""Decode a field log downloaded from the hub (GET /api/field/log) into CSV files.
Usage: python3 hub/tools/field_decode.py field.bin OUTDIR
Writes features.csv, nodes.csv, amps.csv, labels.csv, events.csv, cal.csv, boots.csv, clock.csv, p2p.csv and prints a summary. Record
format: shared/field_format.h. Damaged records are skipped (CRC) and counted. Times are hub uptime in ms; clock.csv maps
uptime to wall-clock time (epoch_ms) once a phone connected."""
import struct, sys, os, csv

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def records(buf):
    i, n, bad = 0, len(buf), 0
    while i < n:
        if buf[i] != 0xA5: i += 1; bad += 1; continue
        if i + 3 > n: break
        ln = buf[i + 2]
        if ln > 80 or i + 4 + ln > n: i += 1; bad += 1; continue
        if crc8(buf[i + 1:i + 3 + ln]) != buf[i + 3 + ln]: i += 1; bad += 1; continue
        yield buf[i + 1], buf[i + 3:i + 3 + ln], bad
        i += 4 + ln; bad = 0

def text(b): return b.decode('utf-8', 'replace')

def main():
    src, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
    buf = open(src, 'rb').read()
    files = {}
    def w(name, header):
        f = open(os.path.join(out, name), 'w', newline=''); c = csv.writer(f); c.writerow(header); files[name] = f; return c
    feat = w('features.csv', ['t_ms', 'node', 'tstd', 'rssi_mean', 'state', 'score', 'moving'])
    node = w('nodes.csv', ['t_ms', 'node', 'online', 'emf_p2p', 'temp_c', 'vbat_mv', 'rssi', 'loss_pct', 'emf_raw'])
    amps = w('amps.csv', ['t_ms', 'node', 'rssi'] + ['a%d' % i for i in range(52)])
    lab = w('labels.csv', ['t_ms', 'label']); ev = w('events.csv', ['t_ms', 'type', 'id', 'text'])
    cal = w('cal.csv', ['t_ms', 'node', 'threshold', 'rssi_base']); boots = w('boots.csv', ['t_ms', 'boot_index', 'tag'])
    clk = w('clock.csv', ['t_ms', 'epoch_ms'])
    pp = w('p2p.csv', ['t_ms', 'rx', 'tx', 'tstd', 'rssi', 'state', 'score', 'threshold', 'rssi_base'])
    cnt, skipped, tmin, tmax = {}, 0, None, None
    for t, p, bad in records(buf):
        skipped += bad; cnt[t] = cnt.get(t, 0) + 1
        tm = struct.unpack_from('<I', p, 0)[0]
        if t != 1: tmin = tm if tmin is None else min(tmin, tm); tmax = tm if tmax is None else max(tmax, tm)
        if t == 1: boots.writerow([tm, struct.unpack_from('<I', p, 4)[0], text(p[8:])])
        elif t == 2:
            _, nd, ts, rs, st, sc, mv = struct.unpack('<IBHhBBB', p); feat.writerow([tm, nd, ts / 1e5, rs / 10, st, sc, mv])
        elif t == 3:
            _, nd, on, p2p, tc, vb, rs, ls = struct.unpack('<IBBHhHbB', p[:14]); raw = struct.unpack_from('<H', p, 14)[0] if len(p) >= 16 else ''
            node.writerow([tm, nd, on, p2p, tc / 100, vb, rs, ls / 10, raw])
        elif t == 4: amps.writerow([tm, p[4], struct.unpack_from('<b', p, 5)[0]] + [round(a / 4, 2) for a in p[6:58]])
        elif t == 5: lab.writerow([tm, text(p[4:])])
        elif t == 6: ev.writerow([tm, p[4], p[5], text(p[6:])])
        elif t == 7: cal.writerow([tm, p[4]] + [round(x, 5) for x in struct.unpack_from('<ff', p, 5)])
        elif t == 8: clk.writerow([tm, struct.unpack_from('<q', p, 4)[0]])
        elif t == 9:
            _, rx, tx, ts, rs, st, sc, th, rb = struct.unpack('<IBBHhBBHh', p); pp.writerow([tm, rx, tx, ts / 1e5, rs / 10, st, sc, th / 1e5, rb / 10])
    for f in files.values(): f.close()
    names = {1: 'boots', 2: 'features', 3: 'nodes', 4: 'amps', 5: 'labels', 6: 'events', 7: 'cal', 8: 'clock', 9: 'p2p'}
    print("records:", ", ".join("%s=%d" % (names[k], v) for k, v in sorted(cnt.items())))
    if tmin is not None: print("span: %.1f min of uptime (t=%d..%d ms); skipped/damaged bytes: %d" % ((tmax - tmin) / 60000.0, tmin, tmax, skipped))
main()
