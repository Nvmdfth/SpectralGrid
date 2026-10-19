#!/usr/bin/env python3
"""EMF wire test analysis (EMF_TEST.md). Usage: python3 hub/tools/emf_analyze.py DECODED_DIR
Reads nodes.csv (needs the emf_raw column) and labels.csv from field_decode.py. Per node and per label: median raw touch level,
shift against that node's own 'emf_quiet' baseline (counts and robust sigmas) and the share of samples with emf_p2p > 100."""
import csv, os, sys, numpy as np
d = sys.argv[1]
N = list(csv.DictReader(open(os.path.join(d, 'nodes.csv')))); L = [(int(r['t_ms']), r['label']) for r in csv.DictReader(open(os.path.join(d, 'labels.csv')))]
if not N or not N[0].get('emf_raw') or N[0]['emf_raw'] == '': sys.exit("nodes.csv has no emf_raw: the log predates the EMF logging")
t = np.array([int(r['t_ms']) for r in N]); nd = np.array([int(r['node']) for r in N]); raw = np.array([int(r['emf_raw']) for r in N]); p2p = np.array([int(r['emf_p2p']) for r in N])
ends = [x[0] for x in L[1:]] + [t[-1]]
segs = [(n, s + 3000, e - 2000) for (s, n), e in zip(L, ends) if e - 2000 > s + 3000]
for node, name in ((0, 'hub'), (1, 'probe')):
    def pick(lbl): return np.concatenate([np.where((t >= a) & (t < b) & (nd == node))[0] for n, a, b in segs if n == lbl] or [np.array([], int)])
    q = pick('emf_quiet')
    if len(q) < 10: print(f"{name}: no emf_quiet samples"); continue
    base = np.median(raw[q]); sig = 1.4826 * np.median(np.abs(raw[q] - base)) + 1.0
    print(f"\n{name} (node {node}): quiet raw median {base:.0f}, robust sigma {sig:.1f}, p2p>100 {100*np.mean(p2p[q]>100):.1f}%  [{len(q)} samples]")
    for lbl in dict.fromkeys(n for n, *_ in segs):
        if not lbl.startswith(name + '_'): continue
        i = pick(lbl)
        if len(i) < 5: continue
        shift = np.median(raw[i]) - base
        verdict = 'RESPONDS' if abs(shift) >= max(50, 5 * sig) else 'no clear response'
        print(f"  {lbl:14s} {len(i):3d} samples  raw median {np.median(raw[i]):7.0f}  shift {shift:+7.0f} counts ({shift/sig:+6.1f} sigma)  p2p>100 {100*np.mean(p2p[i]>100):4.0f}%  -> {verdict}")
