#!/usr/bin/env python3
"""Replay the planned CSI detector over labelled captures (no firmware involved).
Usage: python3 hub/tools/csi_replay.py CAL.csv TEST.csv [TEST2.csv ...]
Calibration = 15 s of the first 'empty' segment of CAL.csv (what a dashboard calibration would capture). The detector then
runs on every labelled segment of every file (CAL.csv's own calibration window is excluded) and is scored per 1 s window.
Detector (all numbers come from the calibration window, nothing is tuned on the test data):
  motion  = temporal std of gain-normalised subcarrier amplitude over 1 s  >  K x calibration p99
  shadow  = mean RSSI over 1 s  <=  calibration median RSSI - SHADOW_DB
  state   = CLEAR | MOTION (motion, no shadow) | CROSSING (motion + shadow) | STILL_ON_LINE (shadow, no motion)"""
import sys, numpy as np
SHADOW_DB = 1.5

def load(path, drop_last=True):
    L = []; T = []; R = []; H = []
    for l in open(path):
        p = l.strip().split(',')
        if p[0] == 'LBL': L.append((int(p[1]), p[2]))
        elif p[0] == 'CSI' and len(p) == 9 and p[2] == '1' and len(p[8]) == 256:
            b = np.frombuffer(bytes.fromhex(p[8]), dtype=np.int8).astype(float)
            T.append(int(p[1])); R.append(int(p[4])); H.append(b[1::2] + 1j * b[0::2])
    use = [k for k in range(1, 64) if not 27 <= k <= 37]
    t = np.array(T); A = np.abs(np.array(H))[:, use]; ends = [x[0] for x in L[1:]] + [t[-1]]
    segs = [(n, s, e) for (s, n), e in zip(L, ends)]
    return t, np.array(R), A, (segs[:-1] if drop_last else segs)

def window_feats(A, R, i0):
    a = A[i0:i0+50]; an = a / np.maximum(a.mean(1, keepdims=True), 1e-6)
    return an.std(0).mean(), R[i0:i0+50].mean()

def windows(t, R, A, s, e, name):
    idx = np.where((t >= s) & (t < e))[0]
    return [window_feats(A, R, idx[i]) + (name,) for i in range(0, len(idx) - 50 + 1, 25)]

cal_path = sys.argv[1]; tests = sys.argv[2:] or [cal_path]
t, R, A, segs = load(cal_path)
n0, s0, e0 = next(x for x in segs if x[0] == 'empty')
cal = windows(t, R, A, s0 + 5000, s0 + 20000, 'empty')
c_t = np.array([w[0] for w in cal]); c_r = np.array([w[1] for w in cal])
thr_base = np.percentile(c_t, 99); rssi_base = np.median(c_r)
print(f"calibration: {len(cal)} windows of 15 s | temporal std median {np.median(c_t):.4f} p99 {thr_base:.4f} | RSSI median {rssi_base:.2f}")

data = []   # (label, motion_raw_stat, rssi, file, round, win_index_in_segment)
for path in tests:
    tt, RR, AA, ss = load(path); rnd = {}; prev = None
    for i, (n, s, e) in enumerate(ss):
        rnd[n] = rnd.get(n, 0) + (1 if prev != n else 0); prev = n
        a, b = s + 5000, e - 2000
        if path == cal_path and n == 'empty' and rnd[n] == 1: a = max(a, s0 + 20000)   # keep the calibration window out of the test
        for w in windows(tt, RR, AA, a, b, n): data.append((n, w[0], w[1], path.split('/')[-1], rnd[n]))
labs = ['empty', 'cross', 'room', 'stand_on', 'sit_off']
labs = [l for l in labs if any(d[0] == l for d in data)]
print("test windows:", {l: sum(1 for d in data if d[0] == l) for l in labs})

def run(K, vote):
    pred = []
    for n, m, r, f, k in data: pred.append((n, m > K * thr_base, r <= rssi_base - SHADOW_DB, f, k))
    if vote:   # persistence: motion needs 2 of the last 3 windows (same file order), cuts one-window flukes
        out = []; hist = []
        for n, m, s, f, k in pred:
            hist.append(m); hist = hist[-3:]; out.append((n, sum(hist) >= 2, s, f, k))
        pred = out
    return pred
def state(m, s): return 'CLEAR' if not (m or s) else ('CROSSING' if m and s else ('MOTION' if m else 'STILL_ON_LINE'))
S = ['CLEAR', 'MOTION', 'CROSSING', 'STILL_ON_LINE']

for K, vote in [(1.0, False), (1.0, True), (1.15, True)]:
    pred = run(K, vote); print(f"\n=== motion threshold = {K} x calibration p99, persistence {'2-of-3' if vote else 'off'}; shadow = RSSI <= base-{SHADOW_DB} dB")
    print(f"{'true activity':14s}{'windows':>8s}" + "".join(f"{s:>16s}" for s in S))
    for l in labs:
        rows = [p for p in pred if p[0] == l]; c = {s: sum(1 for p in rows if state(p[1], p[2]) == s) for s in S}
        print(f"{l:14s}{len(rows):8d}" + "".join(f"{100*c[s]/len(rows):15.0f}%" for s in S))
    e = [p for p in pred if p[0] == 'empty']
    for f in sorted({p[3] for p in e}):
        for k in sorted({p[4] for p in e if p[3] == f}):
            q = [p for p in e if p[3] == f and p[4] == k]; print(f"  empty {f} round {k}: false alarms {100*sum(state(p[1],p[2])!='CLEAR' for p in q)/len(q):.1f}% of {len(q)} windows")
print("\nMotion threshold sweep (per-window detection %, no persistence; empty = false-alarm rate on held-out empty windows):")
mult = [0.9, 1.0, 1.1, 1.25, 1.5, 2.0]
print(f"{'activity':10s}" + "".join(f"{('x%.2f' % k):>8s}" for k in mult))
for l in labs:
    v = np.array([d[1] for d in data if d[0] == l]); print(f"{l:10s}" + "".join(f"{100*np.mean(v > k*thr_base):8.0f}" for k in mult))
