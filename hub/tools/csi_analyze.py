#!/usr/bin/env python3
"""Offline analysis of a csi_capture.py file. Usage: python3 hub/tools/csi_analyze.py CAPTURE.csv [--drop-last]
Segments are cut at LBL lines; the first 5 s (you are still moving into place) and last 2 s of each segment are trimmed.
--drop-last discards the final segment (use when the capture ended without a closing label). Metrics per 1 s window
(50 frames, 50 % overlap), node 1 frames only. A label repeating in order counts as a new round."""
import sys, numpy as np
path = sys.argv[1]; drop_last = '--drop-last' in sys.argv
L = []; T = []; R = []; H = []
for l in open(path):
    p = l.strip().split(',')
    if p[0] == 'LBL': L.append((int(p[1]), p[2]))
    elif p[0] == 'CSI' and len(p) == 9 and p[2] == '1' and len(p[8]) == 256:
        b = np.frombuffer(bytes.fromhex(p[8]), dtype=np.int8).astype(float)
        T.append(int(p[1])); R.append(int(p[4])); H.append(b[1::2] + 1j * b[0::2])
t = np.array(T); rssi = np.array(R); use = [k for k in range(1, 64) if not 27 <= k <= 37]
A = np.abs(np.array(H))[:, use]
print(f"frames {len(t)}  span {(t[-1]-t[0])/1000:.0f}s  rate {len(t)/((t[-1]-t[0])/1000):.1f}/s  median amp {np.median(A):.1f}")
segs = []; rnd = {}
ends = [x[0] for x in L[1:]] + [t[-1]]
for i, ((s, n), e) in enumerate(zip(L, ends)):
    if drop_last and i == len(L) - 1: continue
    rnd[n] = rnd.get(n, 0) + (1 if (i == 0 or L[i-1][1] != n) else 0)
    segs.append((n, rnd[n], s + 5000, e - 2000))
def feats(a, r):
    an = a / np.maximum(a.mean(1, keepdims=True), 1e-6)
    h = len(an) // 2
    return (an.std(0).mean(), 1 - np.corrcoef(an[:h].mean(0), an[h:].mean(0))[0, 1], np.abs(np.diff(an, axis=0)).mean(), a.var(1).mean(), r.mean())
names = ['temporal_std', '1-corr', 'frame_diff', 'spatial_var', 'rssi']
rows = []
for n, k, s, e in segs:
    idx = np.where((t >= s) & (t < e))[0]
    for i in range(0, len(idx) - 50 + 1, 25):
        j = idx[i:i+50]; rows.append((n, k) + feats(A[j], rssi[j]))
labs = list(dict.fromkeys(r[0] for r in rows))
print("windows:", {n: sum(1 for r in rows if r[0] == n) for n in labs})
def col(n, j, k=None): return np.array([r[2+j] for r in rows if r[0] == n and (k is None or r[1] == k)])
print("\nmedian per label and round (empty rounds show baseline drift):")
for j, nm in enumerate(names):
    print(nm)
    for n in labs:
        ks = sorted({r[1] for r in rows if r[0] == n})
        print(f"  {n:9s}", "  ".join(f"r{k}={np.median(col(n,j,k)):8.3f}" for k in ks))
def auc(a, b): return float(np.mean([(x > y) + 0.5 * (x == y) for x in a for y in b]))
print("\nAUC vs empty (pooled rounds):")
for n in labs:
    if n != 'empty': print(f"  {n:9s}", "  ".join(f"{nm}={auc(col(n,j),col('empty',j)):.2f}" for j, nm in enumerate(names)))
print("\nDetection rate above empty's percentile (rssi: below), pooled / per round:")
for pc in (99, 95):
    for j, nm in [(0, 'temporal_std'), (1, '1-corr'), (4, 'rssi')]:
        e = col('empty', j); th = np.percentile(e, 100 - pc if nm == 'rssi' else pc)
        f = (lambda v: v < th) if nm == 'rssi' else (lambda v: v > th)
        out = []
        for n in labs:
            if n == 'empty': continue
            per = [f(col(n, j, k)).mean() for k in sorted({r[1] for r in rows if r[0] == n})]
            out.append(f"{n}={f(col(n,j)).mean():.2f} ({'/'.join(f'{x:.2f}' for x in per)})")
        print(f"p{pc} {nm:12s} thr={th:8.3f}  " + "  ".join(out))
