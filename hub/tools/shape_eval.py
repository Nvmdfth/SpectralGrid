#!/usr/bin/env python3
"""Offline evaluation of a SHAPE-based shadow feature vs the RSSI-drop feature, plus a prototype detector.
Usage: python3 hub/tools/shape_eval.py [features|design|all]   (default all; reads captures/, writes nothing)

Shape feature: each frame's 52 usable LLTF amplitudes divided by the frame mean (or log-amplitude minus its mean), averaged over
a W-second window, compared with a learned reference by RMS difference (optionally after removing a linear tilt across frequency).
Data: field logs (dec/amps.csv, ~25 fps, amplitude already /4) and raw basement sessions (session*.csv, 50 fps, int8 I/Q)."""
import sys, os, csv, numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'captures')
FREQ = np.array(list(range(1, 27)) + list(range(-26, 0)), float)     # amps order a0..a51 = subcarriers +1..+26, -26..-1
USE = [k for k in range(1, 64) if not 27 <= k <= 37]

# ---------------------------------------------------------------- loading
def load_field(name, t_min=0):
    d = os.path.join(ROOT, name, 'dec')
    a = np.genfromtxt(os.path.join(d, 'amps.csv'), delimiter=',', skip_header=1)
    a = a[(a[:, 0] >= t_min) & (a[:, 1] == 1)]
    L = [(int(r['t_ms']), r['label']) for r in csv.DictReader(open(os.path.join(d, 'labels.csv'))) if int(r['t_ms']) >= t_min]
    f = np.genfromtxt(os.path.join(d, 'features.csv'), delimiter=',', skip_header=1)
    f = f[(f[:, 0] >= t_min) & (f[:, 1] == 1)]
    return dict(name=name, t=a[:, 0], rssi=a[:, 2], A=a[:, 3:], L=L, feat=f)

def load_raw(name):
    T, R, A, L = [], [], [], []
    for l in open(os.path.join(ROOT, name)):
        p = l.strip().split(',')
        if p[0] == 'LBL': L.append((int(p[1]), p[2]))
        elif p[0] == 'CSI' and len(p) == 9 and p[2] == '1' and len(p[8]) == 256:
            b = np.frombuffer(bytes.fromhex(p[8]), dtype=np.int8).astype(float)
            T.append(int(p[1])); R.append(int(p[4])); A.append(np.hypot(b[1::2], b[0::2])[USE])
    return dict(name=name, t=np.array(T, float), rssi=np.array(R, float), A=np.array(A), L=L, feat=None)

def segments(D):
    """(label, round, t0, t1): first 15 s of empty / 5 s of others and last 2 s trimmed; last segment trimmed by 30 s."""
    L = D['L']; ends = [x[0] for x in L[1:]] + [D['t'][-1]]; out = []; rnd = {}; prev = None
    for i, ((s, n), e) in enumerate(zip(L, ends)):
        if n == 'room': n = 'near'          # older basement sessions: 'room' = walking around off the line
        rnd[n] = rnd.get(n, 0) + (prev != n); prev = n
        a = s + (15000 if n == 'empty' else 5000); b = e - (30000 if i == len(L) - 1 else 2000)
        if b > a: out.append((n, rnd[n], a, b))
    return out

# ---------------------------------------------------------------- features
def norm_frames(A, mode):
    A = np.maximum(A, 0.25)
    if mode == 'lin': return A / A.mean(1, keepdims=True)
    L = np.log(A); return L - L.mean(1, keepdims=True)

def detrend(S):
    """Remove a linear tilt across frequency (separately per window row)."""
    X = np.vstack([np.ones_like(FREQ), FREQ]).T; P = X @ np.linalg.pinv(X)
    return S - S @ P.T

def windows(D, W=5.0, step=1.0, mode='lin', det=False):
    """Per window: t_end, mean shape (52), mean rssi, temporal std (gain-normalised, over 1 s sub-windows, averaged)."""
    t, N = D['t'], norm_frames(D['A'], mode); r = D['rssi']; Nl = norm_frames(D['A'], 'lin')
    out_t, out_s, out_r, out_ts = [], [], [], []
    e = t[0] + W * 1000
    while e <= t[-1]:
        i0, i1 = np.searchsorted(t, e - W * 1000), np.searchsorted(t, e)
        if i1 - i0 >= W * 15:                      # tolerate drops; ~25 or 50 fps
            out_t.append(e); out_s.append(N[i0:i1].mean(0)); out_r.append(r[i0:i1].mean())
            j0 = np.searchsorted(t, e - 1000); out_ts.append(Nl[j0:i1].std(0).mean() if i1 - j0 > 5 else np.nan)
        e += step * 1000
    S = np.array(out_s)
    if det: S = detrend(S)
    return np.array(out_t), S, np.array(out_r), np.array(out_ts)

def dist(S, ref): return np.sqrt(((S - ref) ** 2).mean(1))

def auc(pos, neg):
    if len(pos) == 0 or len(neg) == 0: return np.nan
    x = np.concatenate([pos, neg]); rk = np.argsort(np.argsort(x)) + 1.0
    # ties are rare with float features; rssi drop ties handled by average below
    from collections import defaultdict
    g = defaultdict(list)
    for i, v in enumerate(x): g[v].append(i)
    for v, ii in g.items():
        if len(ii) > 1: rk[ii] = rk[ii].mean()
    return (rk[:len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg))

def label_of(tw, segs):
    lab = np.array([''] * len(tw), dtype=object)
    for n, k, a, b in segs: lab[(tw >= a) & (tw < b)] = n
    return lab

def calib_ref(tw, S, R, segs, cal_s=30.0):
    """Reference = mean shape and rssi over the first cal_s of the first empty block (as a firmware calibration would)."""
    n, k, a, b = next(s for s in segs if s[0] == 'empty')
    m = (tw >= a) & (tw < a + cal_s * 1000)
    ref = S[m].mean(0); d = dist(S[m], ref)
    med = np.median(d); mad = 1.4826 * np.median(np.abs(d - med))
    return ref, R[m].mean(), m, d

LABS = ['empty', 'cross', 'near', 'far', 'stand_on', 'sit_off']

def eval_dataset(D, W=5.0, mode='lin', det=False, k_thr=None, out=print):
    tw, S, R, TS = windows(D, W=W, mode=mode, det=det)
    segs = segments(D); lab = label_of(tw, segs)
    ref, rref, mcal, dcal = calib_ref(tw, S, R, segs)
    d = dist(S, ref); drop = rref - R
    return dict(tw=tw, S=S, R=R, TS=TS, lab=lab, d=d, drop=drop, mcal=mcal, dcal=dcal, ref=ref, rref=rref, segs=segs)

def feature_table(E, out, shape_thr, rssi_thr=1.5):
    lab, d, drop, mcal = E['lab'], E['d'], E['drop'], E['mcal']
    emp = (lab == 'empty') & ~mcal
    rows = []
    for n in LABS:
        m = (lab == n) & ~mcal
        if m.sum() == 0: continue
        rows.append((n, m.sum(), np.median(d[m]), np.mean(d[m] > shape_thr), auc(d[m], d[emp]) if n != 'empty' else np.nan,
                     np.median(drop[m]), np.mean(drop[m] >= rssi_thr), auc(drop[m], drop[emp]) if n != 'empty' else np.nan))
    out(f"    {'label':9s} {'win':>5s} | {'shape med':>9s} {'det%':>5s} {'AUC':>5s} | {'rssiDrop med':>12s} {'det%':>5s} {'AUC':>5s}")
    for n, c, dm, dr, au, rm, rr, ra in rows:
        out(f"    {n:9s} {c:5d} | {dm:9.4f} {100*dr:5.1f} {au:5.2f} | {rm:12.2f} {100*rr:5.1f} {ra:5.2f}")
    return rows

def thr_from_cal(dcal, k=3.0):
    """Shape threshold from the calibration window alone: k x its p95 distance (calibration windows overlap, so they understate
    the long-run spread; the factor is the margin)."""
    return k * np.percentile(dcal, 95)

# ---------------------------------------------------------------- detector windows (firmware-equivalent inputs)
def fw_windows(D):
    """(t, tstd, rssi_mean) per window. Field logs: the LIVE firmware values (features.csv). Raw sessions: csi_window.h emulated
    (50 frames, step 25, gain-normalised std averaged over subcarriers)."""
    if D['feat'] is not None:
        f = D['feat']; return f[:, 0], f[:, 2], f[:, 3]
    N = norm_frames(D['A'], 'lin'); T, TS, RS = [], [], []
    for e in range(50, len(N) + 1, 25):
        T.append(D['t'][e - 1]); TS.append(N[e - 50:e].std(0).mean()); RS.append(D['rssi'][e - 50:e].mean())
    return np.array(T), np.array(TS), np.array(RS)

def emulate_cal(t, ts, rs, segs, g=0.22, cal_s=30.0):
    """pres_cal_result over the first cal_s of the first empty segment: (q0, thr, rssi_base)."""
    n, k, a, b = next(s for s in segs if s[0] == 'empty')
    m = (t >= a) & (t < min(b, a + cal_s * 1000)); v = ts[m]; med = np.median(v); sig = 1.4826 * np.median(np.abs(v - med))
    thr = max(med * max(1 + g, 1 + 8 * sig / med), 0.015)
    return med, thr, rs[m].mean()

CLEAR, MOTION, ON_LINE = 1, 2, 3
def pop3(h): return bin(h & 7).count('1')

class FwDetector:
    """Python port of shared/presence.h as on calibration-v2 (87e1fd2): 2-of-3 votes, 3 s hold, slow learning in CLEAR,
    5-min stationary-shadow absorb, 60 s upward re-learn. Set absorb=False for the behaviour of the field-log builds (no re-learn)."""
    HOLD, SH_DB, TAU, QUIET, ABS_MS, ABS_DEV, UP_MS, UP_DB, RLIM = 3000, 1.5, 1800e3, 30000, 300000, 0.8, 60000, 0.8, 6.0
    def __init__(s, q0, thr, r0, absorb=True):
        s.q0 = q0; s.ratio = thr / q0; s.thr = thr; s.q = q0; s.r0 = s.r = r0; s.absorb = absorb
        s.mh = s.sh = 0; s.state = CLEAR; s.active = -1e12; s.quiet_since = None; s.last_learn = 0
        s.sh_since = None; s.up_since = None; s.events = []
    def set_r(s, nr, t, why):
        nr = min(max(nr, s.r0 - s.RLIM), s.r0 + s.RLIM); s.events.append((t, why, s.r, nr)); s.r = nr
        s.state = CLEAR; s.mh = s.sh = 0; s.sh_since = s.up_since = None
    def learn(s, ts, rs, t):
        dt = t - s.last_learn; s.last_learn = t
        if dt <= 0 or dt > 5000: return
        a = 1 - np.exp(-dt / s.TAU); s.q += a * (ts - s.q); s.r += a * (rs - s.r)
        s.q = min(max(s.q, 0.7 * s.q0), 1.6 * s.q0); s.r = min(max(s.r, s.r0 - s.RLIM), s.r0 + s.RLIM); s.thr = s.ratio * s.q
    def update(s, t, ts, rs):
        m1 = ts > s.thr; s1 = rs <= s.r - s.SH_DB
        s.mh = ((s.mh << 1) | m1) & 7; s.sh = ((s.sh << 1) | s1) & 7
        mv = pop3(s.mh) >= 2; sh = pop3(s.sh) >= 2
        if s.absorb and sh and not mv:
            if s.sh_since is None: s.sh_since = s.sh_last = t; s.sh_mean = rs; s.sh_dev = 0
            else:
                a = 1 - np.exp(-(t - s.sh_last) / 30000); s.sh_last = t
                s.sh_dev += a * (abs(rs - s.sh_mean) - s.sh_dev); s.sh_mean += a * (rs - s.sh_mean)
            if t - s.sh_since >= s.ABS_MS and s.sh_dev <= s.ABS_DEV: s.set_r(s.sh_mean, t, 'absorb'); return s.state
        else: s.sh_since = None
        if sh or mv:
            s.active = t; s.state = ON_LINE if sh else MOTION; s.quiet_since = None
        elif s.state <= CLEAR or t - s.active >= s.HOLD:
            if s.absorb and rs >= s.r + s.UP_DB:
                if s.up_since is None: s.up_since = t; s.up_mean = rs
                else: s.up_mean += 0.1 * (rs - s.up_mean)
                if t - s.up_since >= s.UP_MS and s.up_mean >= s.r + s.UP_DB: s.set_r(s.up_mean, t, 'up'); return s.state
            else: s.up_since = None
            if s.quiet_since is None: s.quiet_since = t; s.last_learn = t
            elif t - s.quiet_since >= s.QUIET and not m1 and not s1: s.learn(ts, rs, t)
            elif t - s.quiet_since < s.QUIET: s.last_learn = t
        else: s.quiet_since = None
        if s.state > CLEAR and t - s.active >= s.HOLD: s.state = CLEAR
        return s.state

class ProtoDetector(FwDetector):
    """Proposed: a level departure (shadow down >= 1.5 dB, or rise >= 1 dB) is attributed to a person only if motion (2-of-3 tstd
    votes) was seen within ATTR_PRE before its onset or ATTR_POST after. Unattributed departures are level steps (AGC state, TX power,
    temperature, a stale/wrong baseline): once steady for STEP_MS with no motion, the baseline jumps to the new level. Attributed downward
    departures (a person standing on the line) are kept up to LONG_MS of stillness; attributed upward ones re-learn after UP_MS quiet.
    At start (boot / restored baseline) there is no motion history, so a baseline that disagrees with the current level is replaced
    after STEP_MS. Everything else (votes, hold, slow learning in CLEAR) is the firmware's."""
    ATTR_PRE, ATTR_POST, STEP_MS, STEADY, UPA_DB, LONG_MS = 15000, 5000, 20000, 0.8, 1.0, 1200000
    def __init__(s, *a, **k):
        super().__init__(*a, **k); s.uh = 0; s.last_motion = None; s.ep = None; s.eps = []
    def update(s, t, ts, rs):
        m1 = ts > s.thr; s1 = rs <= s.r - s.SH_DB; u1 = rs >= s.r + s.UPA_DB
        s.mh = ((s.mh << 1) | m1) & 7; s.sh = ((s.sh << 1) | s1) & 7; s.uh = ((s.uh << 1) | u1) & 7
        mv = pop3(s.mh) >= 2; sh = pop3(s.sh) >= 2; up = pop3(s.uh) >= 2
        if mv: s.last_motion = t
        dep = -1 if sh else (1 if up else 0)
        if dep:
            e = s.ep
            if e is None or e['dir'] != dep:
                e = s.ep = dict(dir=dep, t0=t, tl=t, mean=rs, dev=0.0,
                                attr=s.last_motion is not None and t - s.last_motion <= s.ATTR_PRE)
                s.eps.append(e)
            else:
                a = 1 - np.exp(-(t - e['tl']) / 5000); e['tl'] = t
                e['dev'] += a * (abs(rs - e['mean']) - e['dev']); e['mean'] += a * (rs - e['mean'])
                if mv and t - e['t0'] <= s.ATTR_POST: e['attr'] = True
            still = t - (s.last_motion if s.last_motion is not None else -1e12)
            need = s.STEP_MS if not e['attr'] else (s.UP_MS if dep > 0 else s.LONG_MS)
            if t - e['t0'] >= need and still >= min(need, s.STEP_MS) and e['dev'] <= s.STEADY:
                s.set_r(e['mean'], t, ('step' if not e['attr'] else ('up' if dep > 0 else 'long')) + ('-' if dep < 0 else '+'))
                s.ep = None; s.uh = 0; return s.state
        else: s.ep = None
        if sh or mv:
            s.active = t; s.state = ON_LINE if sh else MOTION; s.quiet_since = None
        elif s.state <= CLEAR or t - s.active >= s.HOLD:
            if s.quiet_since is None: s.quiet_since = t; s.last_learn = t
            elif t - s.quiet_since >= s.QUIET and not m1 and not s1 and not u1: s.learn(ts, rs, t)
            elif t - s.quiet_since < s.QUIET: s.last_learn = t
        else: s.quiet_since = None
        if s.state > CLEAR and t - s.active >= s.HOLD: s.state = CLEAR
        return s.state

def replay(det, t, ts, rs):
    return np.array([det.update(a, b, c) for a, b, c in zip(t, ts, rs)])

# ---------------------------------------------------------------- reports
CAL_LIVE = {'garage1': (0.03918, -40.00452), 'latency2': (0.03301, -43.00067), 'anomaly1': (0.03782, -34.45874)}

def load_all():
    return {'garage1': load_field('garage1'), 'latency2': load_field('latency2'),
            'session1': load_raw('session1.csv'), 'session2': load_raw('session2.csv')}

def anomaly_report(out):
    AN = load_field('anomaly1', 1938000); t, r = AN['t'], AN['rssi']; N = norm_frames(AN['A'], 'lin')
    f = AN['feat']; out("## A. Anomaly session (t >= 1938 s)\n")
    out(f"- firmware windows: {len(f)}, state counts CLEAR/MOTION/ON_LINE = {[int((f[:,4]==v).sum()) for v in (1,2,3)]}, "
        f"score {f[:,5].min():.0f}..{f[:,5].max():.0f}, tstd max {f[:,2].max():.4f} (thr 0.03782)")
    segs = [(1938, 1958, 'log part 1'), (2063, 2140, 'before step'), (2140, 2165, 'step'), (2165, 2262, 'after step')]
    out("\n| segment | frames | per-frame RSSI counts | fw tstd median / max | shape dist to 'before step' (frame-mean) |")
    out("|---|---|---|---|---|")
    ref = N[(t >= 2063e3) & (t < 2140e3)].mean(0)
    for a, b, nm in segs:
        m = (t >= a * 1e3) & (t < b * 1e3); fm = (f[:, 0] >= a * 1e3) & (f[:, 0] < b * 1e3)
        u, c = np.unique(r[m], return_counts=True)
        out(f"| {nm} {a}-{b} s | {m.sum()} | {', '.join(f'{int(x)}:{y}' for x, y in zip(u, c))} | {np.median(f[fm,2]):.4f} / {f[fm,2].max():.4f} | "
            f"{np.sqrt(((N[m].mean(0)-ref)**2).mean()):.4f} |")
    A0 = N[(t < 2140e3) & (t >= 2063e3) & (r == -36)].mean(0); B0 = N[(t > 2165e3) & (r == -39)].mean(0); dv = B0 - A0
    proj = (N - A0) @ dv / (dv @ dv)
    out("\nPer-frame projection on the step's shape change (0 = before-step shape, 1 = after-step shape), split by the frame's own RSSI:")
    for a, b, nm in segs[1:]:
        cells = []
        for v in (-36, -38, -39):
            m = (t >= a * 1e3) & (t < b * 1e3) & (r == v)
            if m.sum() > 20: cells.append(f"{v} dB: {proj[m].mean():.2f} (n={m.sum()})")
        out(f"- {nm}: " + "; ".join(cells))
    out(f"- shape change of the step: RMS {np.sqrt((dv**2).mean()):.4f}; low-order pattern (+{dv[:6].mean():.3f} near DC on +f, "
        f"{dv[26:30].mean():.3f} at the -26 band edge)")

def feature_report(DS, out):
    out("\n## B. Shape feature vs RSSI drop (reference = first 30 s of the first empty block)\n")
    for mode, det in [('lin', False), ('log', False), ('log', True)]:
        out(f"\n### mode={mode}{' + linear detrend' if det else ''}, 10 s windows, step 1 s\n")
        AN = load_field('anomaly1', 1938000); tw, S, R, _ = windows(AN, W=10, mode=mode, det=det)
        ref = S[(tw >= 2073e3) & (tw < 2103e3)].mean(0); d = dist(S, ref)
        out(f"- anomaly: shape distance before step (2073-2140 s) median {np.median(d[(tw>2073e3)&(tw<2140e3)]):.4f}, "
            f"after step (>2170 s) median {np.median(d[tw>2170e3]):.4f}")
        for nm, D in DS.items():
            E = eval_dataset(D, W=10.0, mode=mode, det=det); thr = thr_from_cal(E['dcal'])
            out(f"\n**{nm}** shape thr from calibration (3 x p95) = {thr:.4f}; RSSI-drop thr 1.5 dB")
            out("```"); feature_table(E, out, thr); out("```")
    out("\n### Quiet-period stability (lin, 10 s windows, distance to the calibration reference, per minute of empty)\n")
    for nm, D in DS.items():
        E = eval_dataset(D, W=10.0); tw, d, R = E['tw'], E['d'], E['R']; cells = []
        for n, k, a, b in E['segs']:
            if n != 'empty': continue
            for s in np.arange(a, b, 60000):
                m = (tw >= s) & (tw < min(s + 60000, b))
                if m.sum() > 5: cells.append(f"{s/60000:.1f}min:{np.median(d[m]):.3f}({R[m].mean():.1f}dB)")
        out(f"- {nm}: " + "  ".join(cells))
    G = DS['garage1']; tw, S, R, TS = windows(G, W=5, step=5); ref = S[(tw > 200e3) & (tw < 330e3)].mean(0); d = dist(S, ref)
    out("\nGarage, empty, 5 s windows around the two quiet RSSI steps (t: shape distance / mean RSSI / tstd):")
    for lo, hi in [(361, 396), (751, 786)]:
        m = (tw >= lo * 1e3) & (tw < hi * 1e3)
        out("- " + "  ".join(f"{a/1000:.0f}s: {b:.3f}/{c:.1f}/{e:.3f}" for a, b, c, e in zip(tw[m], d[m], R[m], TS[m])))

def design_report(DS, out):
    out("\n## C. Detector replay: fw_old (field-log builds, no re-learn), fw_new (presence.h @87e1fd2), proto (motion-attributed level steps)\n")
    AN = load_field('anomaly1', 1938000); t, ts, rs = fw_windows(AN); thr, rb = CAL_LIVE['anomaly1']
    out("### Anomaly replay (baseline -34.46 dB at log start; real hub kept running through the 106 s logging gap)\n")
    for name, det in [('fw_old', FwDetector(thr / 1.22, thr, rb, absorb=False)), ('fw_new', FwDetector(thr / 1.22, thr, rb)),
                      ('proto', ProtoDetector(thr / 1.22, thr, rb))]:
        st = replay(det, t, ts, rs)
        out(f"- {name}: ON_LINE {100*np.mean(st[t<2140e3]==3):.0f}% before step, {100*np.mean(st[t>2170e3]==3):.0f}% after; "
            f"re-learn events {[(round(a/1000,1), w, round(float(b),2), round(float(c),2)) for a, w, b, c in det.events]}")
    out("\n### Labelled sessions: % windows ON_LINE / % any detection (ON_LINE or MOTION)\n")
    out("| data | scenario | detector | " + " | ".join(LABS) + " | recover (s) |"); out("|---|---|---|" + "---|" * (len(LABS) + 1))
    for nm, D in DS.items():
        t, ts, rs0 = fw_windows(D); segs = segments(D); lab = label_of(t, segs)
        q0, thr, rb = emulate_cal(t, ts, rs0, segs)
        if nm in CAL_LIVE: thr, rb = CAL_LIVE[nm]; q0 = thr / 1.22
        e1 = next(s for s in segs if s[0] == 'empty'); nr = next(s for s in segs if s[0] == 'near')
        scen = [('as recorded', 0, None), ('baseline +2 dB (stale, too high)', 2.0, None), ('baseline -3 dB (too low)', -3.0, None),
                ('-3 dB step, quiet', 0, e1[2] + min(25000, (e1[3] - e1[2]) // 2)), ('-3 dB step, during motion', 0, nr[2] + 5000)]
        for sn, off, ts_ in scen:
            rs = rs0 + (np.where(t >= ts_, -3.0, 0) if ts_ else 0)
            for name, det in [('fw_old', FwDetector(q0, thr, rb + off, absorb=False)), ('fw_new', FwDetector(q0, thr, rb + off)),
                              ('proto', ProtoDetector(q0, thr, rb + off))]:
                st = replay(det, t, ts, rs); t0 = ts_ if ts_ else t[0]
                ev = [e for e in det.events if e[0] >= t0]; rec = f"{(ev[0][0]-t0)/1000:.0f}" if (ev and (off or ts_)) else ('-' if not (off or ts_) else 'never')
                cells = [f"{100*np.mean(st[lab==n]==3):.0f}/{100*np.mean(st[lab==n]>1):.0f}" if (lab == n).any() else "" for n in LABS]
                out(f"| {nm} | {sn} | {name} | " + " | ".join(cells) + f" | {rec} |")
        P = ProtoDetector(q0, thr, rb); replay(P, t, ts, rs0)
        dn = [e for e in P.eps if e['dir'] < 0]; so = [e for e in dn if label_of(np.array([e['t0']]), segs)[0] == 'stand_on']
        out(f"| {nm} | attribution | proto | down episodes {len(dn)}, attributed {sum(e['attr'] for e in dn)}; stand_on onsets attributed "
            f"{sum(e['attr'] for e in so)}/{len(so)} |" + " |" * (len(LABS) - 1) + " |")

def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    DS = load_all() if what in ('all', 'features', 'design') else {}
    if what in ('all', 'anomaly'): anomaly_report(print)
    if what in ('all', 'features'): feature_report(DS, print)
    if what in ('all', 'design'): design_report(DS, print)

if __name__ == '__main__': main()
