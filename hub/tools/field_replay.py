#!/usr/bin/env python3
"""Replay the presence detector (shared/csi_window.h + shared/presence.h) over labelled data and sweep its parameters.

Usage:
  python3 hub/tools/field_replay.py [--garage captures/garage1/dec] [--basement captures/session1.csv captures/session2.csv]
                                    [--out captures/replay_report.md] [--csv captures/replay_sweep.csv]

Inputs
  garage    a decoded field log (hub/tools/field_decode.py): amps.csv (amplitude x4 bytes, decoded back to amplitude), labels.csv,
            features.csv/cal.csv (the LIVE firmware output, used only for the validation).
  basement  raw I/Q captures from hub/tools/csi_capture.py (CSI + LBL lines), node 1 only.

Timing. The firmware's window is 50 frames (1 s at 50 fps), a result every 25 frames. The field log keeps every 2nd frame (~25 fps),
so on garage data a window is 25 frames and a result comes every 12.5 frames (12, 13, 12, 13...): the same 1 s / 0.5 s timing but
half the frames per window. Basement captures are replayed twice: 50 fps with the firmware's exact windows ("basement50") and
subsampled to every 2nd frame with the garage's windows ("basement25", the like-for-like comparison with the garage).
The amplitude bytes are amplitude x 4 (0.25 resolution), a little coarser than the firmware's floats: validated below.

Detector emulation (presence.h): temporal std of gain-normalised amplitudes, mean RSSI of the window, calibration threshold =
max(quiet-window std, 0.015) x margin, baseline RSSI = calibration mean, m-of-n vote on motion and on shadow, ON_LINE (shadow) >
MOTION > CLEAR, state held `hold` seconds after the last active window.

Segments run from one label to the next. Trims: first 5 s of a segment (15 s for 'empty': the person is walking out), last 2 s;
the last garage segment loses 30 s (walking back in); the last basement segment is dropped (no closing label)."""
import sys, os, csv, itertools, argparse
import numpy as np

USE = [k for k in range(1, 64) if not 27 <= k <= 37]
THR_FLOOR = 0.015
CLEAR, MOTION, ON_LINE = 1, 2, 3
CURRENT = dict(margin=1.15, vote=(2, 3), hold=4.0, shadow=1.5)


# ----------------------------------------------------------------------------------------------- loading
def load_garage(d):
    a = np.loadtxt(os.path.join(d, 'amps.csv'), delimiter=',', skiprows=1)
    a = a[a[:, 1] == 1]
    labels = [(int(r['t_ms']), r['label']) for r in csv.DictReader(open(os.path.join(d, 'labels.csv')))]
    feats = [r for r in csv.DictReader(open(os.path.join(d, 'features.csv'))) if int(r['node']) == 1]
    cal = [r for r in csv.DictReader(open(os.path.join(d, 'cal.csv'))) if int(r['node']) == 1]
    return dict(t=a[:, 0], rssi=a[:, 2], amp=a[:, 3:55], labels=labels, feats=feats, cal=cal)


def load_iq(path):
    T, R, A, L = [], [], [], []
    for line in open(path):
        p = line.strip().split(',')
        if p[0] == 'LBL':
            L.append((int(p[1]), p[2]))
        elif p[0] == 'CSI' and len(p) == 9 and p[2] == '1' and len(p[8]) == 256:
            b = np.frombuffer(bytes.fromhex(p[8]), dtype=np.int8).astype(float)
            T.append(int(p[1])); R.append(int(p[4])); A.append(np.hypot(b[1::2], b[0::2])[USE])
    return dict(t=np.array(T), rssi=np.array(R, float), amp=np.array(A), labels=L)


# --------------------------------------------------------------------------------------------- windows
def make_windows(d, win, step):
    """step may be fractional (12.5 -> 12, 13, 12, 13...). Returns arrays (t_end_ms, tstd, rssi_mean)."""
    an = d['amp'] / np.maximum(d['amp'].mean(1, keepdims=True), 1e-6)
    t, r = d['t'], d['rssi']
    ends = []; k = 0
    while True:
        e = int(np.floor(win + step * k))
        if e > len(t): break
        ends.append(e); k += 1
    ts = np.array([an[e - win:e].std(0).mean() for e in ends])
    rs = np.array([r[e - win:e].mean() for e in ends])
    te = np.array([t[e - 1] for e in ends])
    return te, ts, rs


def subsample(d, n=2):
    return dict(t=d['t'][::n], rssi=d['rssi'][::n], amp=d['amp'][::n], labels=d['labels'])


def segments(labels, t_end, drop_last, last_trim_ms):
    """-> list of (label, round, a_ms, b_ms) after trims."""
    ends = [x[0] for x in labels[1:]] + [t_end]
    out = []; rnd = {}; prev = None
    for i, ((s0, n), e0) in enumerate(zip(labels, ends)):
        rnd[n] = rnd.get(n, 0) + (1 if prev != n else 0); prev = n
        last = i == len(labels) - 1
        if last and drop_last: continue
        a = s0 + (15000 if n == 'empty' else 5000); b = e0 - (last_trim_ms if last else 2000)
        if b > a: out.append((n, rnd[n], a, b))
    return out


# ------------------------------------------------------------------------------------------- detector
class Detector:
    """presence.h with a configurable vote and hold."""
    def __init__(self, thr, rssi_base, vote=(2, 3), hold=4.0, shadow=1.5):
        self.thr, self.rb, self.m, self.n, self.hold, self.sh_db = thr, rssi_base, vote[0], vote[1], hold * 1000.0, shadow
        self.mh, self.sh, self.state, self.active = [], [], CLEAR, -1e18

    def update(self, tstd, rssi, now):
        self.mh = (self.mh + [tstd > self.thr])[-self.n:]
        self.sh = (self.sh + [rssi <= self.rb - self.sh_db])[-self.n:]
        mv, sh = sum(self.mh) >= self.m, sum(self.sh) >= self.m
        if sh or mv:
            self.active = now; self.state = ON_LINE if sh else MOTION
        elif self.state > CLEAR and now - self.active >= self.hold:
            self.state = CLEAR
        return self.state


def calibrate(tstd, rssi, margin):
    return max(float(np.max(tstd)), THR_FLOOR) * margin, float(np.mean(rssi))


def run(te, ts, rs, thr, rb, vote, hold, shadow):
    det = Detector(thr, rb, vote, hold, shadow)
    return np.array([det.update(a, b, c) for a, b, c in zip(ts, rs, te)])


# ------------------------------------------------------------------------------------------- datasets
class Dataset:
    """windows + labelled test segments + calibration windows, for one environment/pipeline."""
    def __init__(self, name, te, ts, rs, labels, segs, cal_a, cal_b, extra=None):
        self.name, self.te, self.ts, self.rs, self.labels, self.segs = name, te, ts, rs, labels, segs
        self.cal_idx = np.where((te >= cal_a) & (te < cal_b))[0]
        self.extra = extra or []          # further (te, ts, rs, segs) tuples replayed with the same calibration (basement session1)

    def evaluate(self, margin, vote, hold, shadow):
        thr, rb = calibrate(self.ts[self.cal_idx], self.rs[self.cal_idx], margin)
        parts = [(self.te, self.ts, self.rs, self.segs, self.cal_idx)] + [(e[0], e[1], e[2], e[3], np.array([], int)) for e in self.extra]
        det_cnt = {}; tot = {}; split = [0, 0, 0]; fa_w = fa_n = on = 0
        for te, ts, rs, segs, cal_idx in parts:
            st = run(te, ts, rs, thr, rb, vote, hold, shadow)
            skip = set(cal_idx.tolist())
            for lab, k, a, b in segs:
                idx = [i for i in np.where((te >= a) & (te < b))[0] if i not in skip]
                if not idx: continue
                s = st[idx]
                tot[lab] = tot.get(lab, 0) + len(idx); det_cnt[lab] = det_cnt.get(lab, 0) + int(np.sum(s > CLEAR))
                if lab == 'cross':
                    for j, v in enumerate((CLEAR, MOTION, ON_LINE)): split[j] += int(np.sum(s == v))
                if lab == 'empty':
                    fa_w += int(np.sum(s > CLEAR)); fa_n += len(idx)
                    on += int(np.sum(np.diff((s > CLEAR).astype(int)) == 1))
        res = dict(thr=thr, rb=rb, windows=tot, fa_rate=100.0 * fa_w / max(fa_n, 1), onsets=on, onsets_10min=on / max(fa_n * 0.5 / 600.0, 1e-9),
                   empty_windows=fa_n)
        for lab in tot:
            if lab != 'empty': res['det_' + lab] = 100.0 * det_cnt[lab] / tot[lab]
        if sum(split): res['cross_clear'], res['cross_motion'], res['cross_online'] = [100.0 * v / sum(split) for v in split]
        return res


def build_garage(g):
    te, ts, rs = make_windows(g, 25, 12.5)
    segs = segments(g['labels'], g['t'][-1], False, 30000)
    e0 = next(s for s in segs if s[0] == 'empty')
    return Dataset('garage', te, ts, rs, g['labels'], segs, e0[2] + 15000 * 0, e0[2] + 15000), (te, ts, rs, segs)


def build_basement(paths, name, sub):
    ds = [load_iq(p) for p in paths]
    if sub: ds = [subsample(d) for d in ds]
    win, step = (25, 12.5) if sub else (50, 25)
    parts = []
    for d in ds:
        te, ts, rs = make_windows(d, win, step)
        parts.append((te, ts, rs, segments(d['labels'], d['t'][-1], True, 0)))
    cal = paths.index(next(p for p in paths if p.endswith('session2.csv'))) if any(p.endswith('session2.csv') for p in paths) else 0
    te, ts, rs, segs = parts[cal]
    e0 = next(s for s in segs if s[0] == 'empty')
    raw_e0 = next(t for t, n in ds[cal]['labels'] if n == 'empty')
    extra = [parts[i] for i in range(len(parts)) if i != cal]
    return Dataset(name, te, ts, rs, ds[cal]['labels'], segs, raw_e0 + 5000, raw_e0 + 20000, extra)


# ------------------------------------------------------------------------------------------- validation
def validate(g):
    te, ts, rs = make_windows(g, 25, 12.5)
    cal = g['cal'][0]; thr, rb = float(cal['threshold']), float(cal['rssi_base'])
    live_t = np.array([int(r['t_ms']) for r in g['feats']]); live_ts = np.array([float(r['tstd']) for r in g['feats']])
    live_st = np.array([int(r['state']) for r in g['feats']])
    st = run(te, ts, rs, thr, rb, (2, 3), 4.0, 1.5)
    j = np.searchsorted(te, live_t); j = np.clip(j, 1, len(te) - 1)
    j = np.where(np.abs(te[j - 1] - live_t) < np.abs(te[j] - live_t), j - 1, j)
    ok = np.abs(te[j] - live_t) <= 300
    skip = live_t < live_t[0] + 15000
    m = ok & ~skip
    agree = np.mean(st[j][m] == live_st[m]) * 100
    binag = np.mean((st[j][m] > CLEAR) == (live_st[m] > CLEAR)) * 100
    ratio = np.median(ts[j][m] / live_ts[m]); corr = np.corrcoef(ts[j][m], live_ts[m])[0, 1]
    rows = [("windows compared", int(m.sum())), ("state agreement (CLEAR/MOTION/ON_LINE)", f"{agree:.1f}%"),
            ("agreement clear vs non-clear", f"{binag:.1f}%"), ("replay/live temporal std (median ratio)", f"{ratio:.3f}"),
            ("correlation of tstd", f"{corr:.3f}")]
    # confusion for the disagreements
    conf = {(a, b): int(np.sum((st[j][m] == a) & (live_st[m] == b))) for a in (1, 2, 3) for b in (1, 2, 3)}
    return rows, conf, (thr, rb)


# ------------------------------------------------------------------------------------------- reporting
def fmt(res, labs):
    cells = [f"{res['fa_rate']:.1f}%", f"{res['onsets_10min']:.1f}"]
    for l in labs: cells.append(f"{res['det_' + l]:.0f}%" if 'det_' + l in res else '-')
    if 'cross_motion' in res: cells.append(f"{res['cross_motion']:.0f}/{res['cross_online']:.0f}")
    else: cells.append('-')
    return cells


def table(title, header, rows):
    out = [f"**{title}**", "", "| " + " | ".join(header) + " |", "| " + " | ".join(['---'] * len(header)) + " |"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out) + "\n"


def garage_calibration_robustness(gds, n_cal=60, seed=0):
    """Calibrate on 60 different random 15 s windows of the garage's 10 min quiet block (30 windows each), then measure the false-alarm
    rate on every OTHER empty window (rest of that block, the later empty blocks) and the detection of far / sit_off. Shows how often a
    15 s calibration under-estimates the quiet spread, i.e. how much margin is needed. Current vote/hold/shadow settings."""
    rng = np.random.default_rng(seed)
    segs = gds.segs; first = next(sg for sg in segs if sg[0] == 'empty')
    blk = np.where((gds.te >= first[2]) & (gds.te < first[3]))[0]
    starts = rng.choice(np.arange(blk[0], blk[-1] - 30), n_cal, replace=False)
    empties = np.concatenate([np.where((gds.te >= a) & (gds.te < b))[0] for l, k, a, b in segs if l == 'empty'])
    far = np.concatenate([np.where((gds.te >= a) & (gds.te < b))[0] for l, k, a, b in segs if l == 'far'])
    sit = np.concatenate([np.where((gds.te >= a) & (gds.te < b))[0] for l, k, a, b in segs if l == 'sit_off'])
    rows = []
    for mg in (1.00, 1.05, 1.10, 1.15, 1.20, 1.30):
        fa, fr_, sr = [], [], []
        for s0 in starts:
            ci = np.arange(s0, s0 + 30)
            thr, rb = calibrate(gds.ts[ci], gds.rs[ci], mg)
            st = run(gds.te, gds.ts, gds.rs, thr, rb, CURRENT['vote'], CURRENT['hold'], CURRENT['shadow'])
            ev = np.setdiff1d(empties, np.arange(s0 - 12, s0 + 42))        # keep the calibration window and its hold tail out of the test
            fa.append(100.0 * np.mean(st[ev] > CLEAR)); fr_.append(100.0 * np.mean(st[far] > CLEAR)); sr.append(100.0 * np.mean(st[sit] > CLEAR))
        rows.append([f"{mg:.2f}", f"{np.mean(fa):.1f}%", f"{np.percentile(fa, 95):.1f}%", f"{np.max(fa):.1f}%", f"{100.0 * np.mean(np.array(fa) > 1.0):.0f}%", f"{np.mean(fr_):.0f}%", f"{np.mean(sr):.0f}%"])
    return "## 6. Does the 15 s calibration window matter? (garage, %d random windows from the 10 min quiet block; test = all other empty windows, %d)\n\n" % (n_cal, len(empties)) + \
        table("Margin vs false alarms over different calibration windows (current vote/hold/shadow)", ["margin", "FA mean", "FA p95", "FA worst", "calibrations with FA > 1 %", "far detection", "sit_off detection"], rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--garage', default='captures/garage1/dec')
    ap.add_argument('--basement', nargs='*', default=['captures/session1.csv', 'captures/session2.csv'])
    ap.add_argument('--out', default='captures/replay_report.md'); ap.add_argument('--csv', default='captures/replay_sweep.csv')
    a = ap.parse_args()

    g = load_garage(a.garage)
    gds, _ = build_garage(g)
    b25 = build_basement(a.basement, 'basement25', True)
    b50 = build_basement(a.basement, 'basement50', False)
    sets = [gds, b25, b50]
    rep = ["# Replay comparison: garage field log vs basement captures\n"]

    rows, conf, (thr, rb) = validate(g)
    rep.append(table(f"1. Validation: Python replay vs the LIVE firmware states (garage log, thr {thr:.5f}, RSSI base {rb:.4f}, 2-of-3, 4 s, 1.5 dB; first 15 s skipped)", ["metric", "value"], rows))
    names = {1: 'CLEAR', 2: 'MOTION', 3: 'ON_LINE'}
    rep.append(table("Confusion (rows = replay, columns = live firmware), window counts", ["replay \\ live"] + list(names.values()),
                     [[names[a_]] + [conf[(a_, b_)] for b_ in (1, 2, 3)] for a_ in (1, 2, 3)]))

    info = []
    for d in sets:
        e = d.evaluate(**CURRENT)
        info.append([d.name, f"{e['thr']:.4f}", f"{e['rb']:.2f}", e['empty_windows'], ", ".join(f"{k} {v}" for k, v in e['windows'].items() if k != 'empty')])
    rep.append(table("2. Calibration in place (15 s quiet window, margin 1.15) and test sizes (windows of 0.5 s step; empty = false-alarm denominator)",
                     ["dataset", "threshold", "baseline RSSI", "empty windows", "test windows per activity"], info))

    def labs_of(d): return ['cross', 'near', 'far', 'stand_on', 'sit_off'] if d.name == 'garage' else ['cross', 'room', 'stand_on', 'sit_off']
    hdr = lambda d: ['FA win%', 'FA onsets/10min'] + labs_of(d) + ['cross MOTION/ON_LINE %']
    rep.append("## 3. Current settings (margin 1.15, 2-of-3, hold 4 s, shadow 1.5 dB)\n")
    for d in sets:
        rep.append(table(d.name, hdr(d), [fmt(d.evaluate(**CURRENT), labs_of(d))]))

    # --- sweep
    margins = [round(1.0 + 0.05 * i, 2) for i in range(11)]
    votes = [(1, 2), (2, 3), (3, 5)]; holds = [2.0, 4.0, 6.0]; shadows = [1.0, 1.5, 2.0, 3.0]
    results = {}
    for mg, vt, hd, sd in itertools.product(margins, votes, holds, shadows):
        results[(mg, vt, hd, sd)] = [d.evaluate(mg, vt, hd, sd) for d in sets]
    with open(a.csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['margin', 'vote', 'hold_s', 'shadow_db'] + [f"{d.name}_{k}" for d in sets for k in ('fa_rate', 'onsets_10min', 'det_cross', 'det_near_or_room', 'det_far', 'det_stand_on', 'det_sit_off', 'cross_motion', 'cross_online')])
        for k, rr in results.items():
            row = [k[0], f"{k[1][0]}of{k[1][1]}", k[2], k[3]]
            for e in rr:
                row += [f"{e['fa_rate']:.2f}", f"{e['onsets_10min']:.2f}", f"{e.get('det_cross', 0):.1f}", f"{e.get('det_near', e.get('det_room', 0)):.1f}", f"{e.get('det_far', 0):.1f}",
                        f"{e.get('det_stand_on', 0):.1f}", f"{e.get('det_sit_off', 0):.1f}", f"{e.get('cross_motion', 0):.1f}", f"{e.get('cross_online', 0):.1f}"]
            w.writerow(row)

    def one_at_a_time(title, key_idx, values, label):
        rep.append(f"### {title}\n")
        for di, d in enumerate(sets[:2]):          # garage and the like-for-like basement
            rows = []
            for v in values:
                k = list(CURRENT.values()); k[key_idx] = v
                e = results[tuple(k)][di]
                rows.append([label(v)] + fmt(e, labs_of(d)))
            rep.append(table(d.name, [title.split(' ')[0].lower()] + hdr(d), rows))
    rep.append("## 4. One parameter at a time (others at the current setting)\n")
    one_at_a_time("Margin sweep", 0, margins, lambda v: f"{v:.2f}")
    one_at_a_time("Vote sweep", 1, votes, lambda v: f"{v[0]}-of-{v[1]}")
    one_at_a_time("Hold sweep (s)", 2, holds, lambda v: f"{v:.0f}")
    one_at_a_time("Shadow sweep (dB)", 3, shadows, lambda v: f"{v:.1f}")

    # --- robust selection: both datasets must stay at <= FA_MAX window false-alarm rate and <= ONS_MAX onsets/10 min
    def score(es):
        s = []
        for e in es[:2]:
            near = e.get('det_near', e.get('det_room', 0)); far = e.get('det_far', near)
            s.append(np.mean([e.get('det_cross', 0), (near + far) / 2, e.get('det_stand_on', 0)]))
        return min(s), np.mean(s)
    cand = []
    for k, es in results.items():
        if all(e['fa_rate'] <= 1.0 for e in es[:2]):
            worst, mean = score(es); cand.append((worst, mean, k, es))
    cand.sort(key=lambda x: (-x[0], -x[1]))
    rep.append("## 5. Robust settings: false-alarm window rate <= 1 % in BOTH garage and basement25, ranked by the worse environment's mean detection of cross / walk (near+far or room) / stand_on\n")
    rows = []
    cur_key = (1.15, (2, 3), 4.0, 1.5)
    for worst, mean, k, es in cand[:12]:
        g_, b_ = es[0], es[1]
        rows.append([f"{k[0]:.2f}", f"{k[1][0]}-of-{k[1][1]}", f"{k[2]:.0f}", f"{k[3]:.1f}", f"{worst:.0f}", f"{g_['fa_rate']:.1f}/{g_['onsets_10min']:.1f}", f"{b_['fa_rate']:.1f}/{b_['onsets_10min']:.1f}",
                     f"{g_.get('det_sit_off', 0):.0f}/{b_.get('det_sit_off', 0):.0f}", f"{g_.get('cross_online', 0):.0f}/{b_.get('cross_online', 0):.0f}"])
    rep.append(table("Top 12", ["margin", "vote", "hold s", "shadow dB", "worst-env detection %", "garage FA% / onsets per 10 min", "basement25 FA% / onsets", "sit_off % (garage/basement)", "cross ON_LINE % (garage/basement)"], rows))
    es = results[cur_key]
    rep.append(f"Current settings for reference: worst-env detection {score(es)[0]:.0f}, garage FA {es[0]['fa_rate']:.1f}% / {es[0]['onsets_10min']:.1f} onsets per 10 min, basement25 FA {es[1]['fa_rate']:.1f}% / {es[1]['onsets_10min']:.1f}.\n")
    # --- how much does the 15 s calibration window matter? (garage only: the 10 min quiet block gives many windows to choose from)
    rep.append(garage_calibration_robustness(gds))
    open(a.out, 'w').write("\n".join(rep))
    print("\n".join(rep))


if __name__ == '__main__':
    main()
