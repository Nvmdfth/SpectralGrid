#!/usr/bin/env python3
"""Summarise a decoded field log (hub/tools/field_decode.py output dir): quiet baseline, per-activity detector results, shadow run
lengths, EMF per activity, false alarms. Usage: python3 hub/tools/field_analyze.py DECODED_DIR [--link 1]
Segments run from one label to the next; the first 5 s (15 s for 'empty': you are walking out) and the last 2 s are trimmed; the
last segment is trimmed by 30 s (you are walking back in)."""
import sys, csv, os, numpy as np
d = sys.argv[1]; link = int(sys.argv[sys.argv.index('--link') + 1]) if '--link' in sys.argv else 1
def rows(n): return list(csv.DictReader(open(os.path.join(d, n))))
F = [r for r in rows('features.csv') if int(r['node']) == link]
t = np.array([int(r['t_ms']) for r in F]); ts = np.array([float(r['tstd']) for r in F]); rs = np.array([float(r['rssi_mean']) for r in F])
st = np.array([int(r['state']) for r in F]); mv = np.array([int(r['moving']) for r in F]); sc = np.array([int(r['score']) for r in F])
L = [(int(r['t_ms']), r['label']) for r in rows('labels.csv')]
N = rows('nodes.csv'); end_t = t[-1]
ends = [x[0] for x in L[1:]] + [end_t]
segs = []; rnd = {}; prev = None
for i, ((s0, n), e0) in enumerate(zip(L, ends)):
    rnd[n] = rnd.get(n, 0) + (1 if prev != n else 0); prev = n
    a = s0 + (15000 if n == 'empty' else 5000); b = e0 - (30000 if i == len(L) - 1 else 2000)
    if b > a: segs.append((n, rnd[n], a, b))
print(f"link {link}: {len(t)} windows over {(t[-1]-t[0])/60000:.1f} min; calibrated windows (state>0): {100*np.mean(st>0):.0f}%")
print("segments:", ", ".join(f"{n}{k}[{(b-a)/1000:.0f}s]" for n, k, a, b in segs))

def sel(n, k=None): return np.concatenate([np.where((t >= a) & (t < b))[0] for nn, kk, a, b in segs if nn == n and (k is None or kk == k)] or [np.array([], int)])
print("\nQUIET BASELINE (empty blocks): temporal std / mean RSSI / detector output")
for k in sorted({kk for nn, kk, *_ in segs if nn == 'empty'}):
    i = sel('empty', k)
    if len(i) < 10: continue
    print(f"  empty #{k}: {len(i):4d} windows  tstd median {np.median(ts[i]):.4f} p99 {np.percentile(ts[i],99):.4f} max {ts[i].max():.4f} | RSSI mean {rs[i].mean():6.2f} sd {rs[i].std():.2f} | non-clear {100*np.mean(st[i]>1):4.1f}%")
print("\nDETECTOR STATE per activity (live firmware output)  % CLEAR / MOTION / ON_LINE (on-line moving)")
for n in dict.fromkeys(x[1] for x in L):
    i = sel(n)
    if len(i) < 5: continue
    c = [100 * np.mean(st[i] == v) for v in (1, 2, 3)]
    print(f"  {n:9s} {len(i):4d} win  CLEAR {c[0]:3.0f}%  MOTION {c[1]:3.0f}%  ON_LINE {c[2]:3.0f}% ({100*np.mean((st[i]==3)&(mv[i]==1)):3.0f}% moving)  tstd med {np.median(ts[i]):.4f}  RSSI med {np.median(rs[i]):6.1f}")
print("\nSHADOW RUN LENGTHS (RSSI window <= base-1.5 dB; cross should be short bursts, stand_on one long run)")
base = np.median(rs[sel('empty')]) if len(sel('empty')) else -40
for n in ('cross', 'near', 'far', 'stand_on', 'sit_off'):
    for k in sorted({kk for nn, kk, *_ in segs if nn == n}):
        i = sel(n, k)
        if len(i) < 10: continue
        sh = rs[i] <= base - 1.5; runs = []; c = 0
        for v in sh:
            if v: c += 1
            else:
                if c: runs.append(c * 0.5); c = 0
        if c: runs.append(c * 0.5)
        print(f"  {n:9s}#{k}: shadowed {100*sh.mean():3.0f}% of {len(i)*0.5:3.0f}s, bursts {len(runs):2d}, median {np.median(runs) if runs else 0:4.1f}s, longest {max(runs) if runs else 0:4.1f}s, min RSSI {rs[i].min():6.1f}")
print("\nEMF p2p and temperature by activity (nodes.csv)  node0=hub node1=probe")
nt = np.array([int(r['t_ms']) for r in N]); nn_ = np.array([int(r['node']) for r in N]); p2p = np.array([int(r['emf_p2p']) for r in N]); tc = np.array([float(r['temp_c']) for r in N])
for node in (0, 1):
    print(f"  node {node}:")
    for n in dict.fromkeys(x[1] for x in L):
        idx = np.concatenate([np.where((nt >= a) & (nt < b) & (nn_ == node))[0] for q, k, a, b in segs if q == n] or [np.array([], int)])
        if len(idx) > 5: print(f"    {n:9s} p2p median {np.median(p2p[idx]):4.0f} p95 {np.percentile(p2p[idx],95):4.0f} max {p2p[idx].max():4d}   temp {tc[idx].mean():5.2f}")
print("\nFALSE ALARM onsets (state>1 starting inside empty blocks)")
for k in sorted({kk for nn, kk, *_ in segs if nn == 'empty'}):
    i = sel('empty', k)
    if len(i) < 10: continue
    a = (st[i] > 1).astype(int); on = int(np.sum(np.diff(a) == 1) + a[0]); print(f"  empty #{k}: {on} onsets in {len(i)*0.5/60:.1f} min")
