#!/usr/bin/env python3
"""Guided multi-link walk test: labelled raw-CSI capture on the hub UART, then a per-link and position analysis.
  python3 hub/tools/walktest.py run OUT.csv [PORT]     capture on a fixed schedule (labels are sent at the step times)
  python3 hub/tools/walktest.py record OUT.csv [PORT]  open-ended capture; labels come from the dashboard (Labels panel); stop: touch OUT.csv.stop or Ctrl-C/SIGINT
  python3 hub/tools/walktest.py analyze OUT.csv        per-link response per label + how well the spots can be told apart
Square layout assumed by the schedule: hub bottom-left, probes at the other three corners. Like csi_capture.py this
never sets DTR/RTS (that resets the board) and returns the UART to 115200 on exit."""
import subprocess, sys, time

STEP_S = 40          # each step: ~8 s to get into place (trimmed in analysis), the rest held
SCHEDULE = [         # (label, what the person does)
    ("empty",       "stand OUTSIDE the room / out of sight of every link"),
    ("center",      "stand still in the middle of the square"),
    ("mid_bottom",  "stand still at the midpoint of the bottom side (hub corner <-> bottom-right)"),
    ("mid_left",    "stand still at the midpoint of the left side (hub corner <-> top-left)"),
    ("mid_top",     "stand still at the midpoint of the top side"),
    ("mid_right",   "stand still at the midpoint of the right side"),
    ("near_hub",    "stand still just inside the square, 1 m diagonally in from the hub corner"),
    ("near_far",    "stand still just inside the square, 1 m diagonally in from the top-right corner"),
    ("walk_bottom", "walk slowly back and forth along the bottom side"),
    ("walk_left",   "walk slowly back and forth along the left side"),
    ("walk_top",    "walk slowly back and forth along the top side"),
    ("walk_cross",  "walk slowly back and forth along the top-left <-> bottom-right diagonal"),
    ("walk_free",   "walk randomly around inside the square"),
    ("empty2",      "stand OUTSIDE the room again (drift check)"),
]

def run(out, port):
    import serial
    subprocess.run(["stty", "-F", port, "115200", "raw", "-echo", "-hupcl"], check=True)
    ser = serial.Serial(port, 115200, timeout=0.2)
    def send(s): ser.write((s + "\n").encode()); ser.flush()
    def wait_for(sub, t):
        end = time.time() + t
        while time.time() < end:
            if sub in ser.readline().decode("utf-8", "replace"): return True
        return False
    n = {"CSI": 0, "LBL": 0}
    try:
        ser.reset_input_buffer(); send(""); send("csicap on")
        if not wait_for("csicap on", 5): sys.exit("hub did not acknowledge `csicap on`")
        time.sleep(0.3); ser.baudrate = 921600; ser.reset_input_buffer()
        t0 = time.time(); step = -1
        print(f"START (total {len(SCHEDULE)*STEP_S} s)", flush=True)
        with open(out, "w") as f:
            while step < len(SCHEDULE) or time.time() - t0 < len(SCHEDULE) * STEP_S + 1:
                due = int((time.time() - t0) // STEP_S)
                if due > step and step < len(SCHEDULE):
                    step += 1
                    if step < len(SCHEDULE):
                        send("csilabel " + SCHEDULE[step][0]); print(f"{time.time()-t0:5.0f}s  {SCHEDULE[step][0]}: {SCHEDULE[step][1]}", flush=True)
                if time.time() - t0 >= len(SCHEDULE) * STEP_S: break
                l = ser.readline().decode("utf-8", "replace").strip()
                k = l[:3]
                if k in ("CSI", "LBL", "CAP") and l.count(",") >= 2:
                    f.write(l + "\n")
                    if k in n: n[k] += 1
    finally:
        try:
            ser.baudrate = 921600; send("csicap off"); time.sleep(0.5); ser.baudrate = 115200
        except Exception: pass
        print(f"done: frames={n['CSI']} labels={n['LBL']} -> {out}", flush=True)

PAGE = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1"><title>Walk labels</title>
<style>body{font:16px system-ui;margin:12px;background:#111;color:#eee}h3{margin:10px 0 4px}
#g{display:grid;grid-template-columns:repeat(5,1fr);gap:4px;max-width:420px;margin:auto}
#g button{aspect-ratio:1;font-size:12px}button{font:inherit;padding:10px;border-radius:6px;border:1px solid #777;background:#222;color:#eee}
button.on{background:#0a6;border-color:#0f9}#cur{font-size:20px;margin:8px 0;min-height:1.4em}#st{color:#999;font-size:13px}
.row{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0}</style>
<div id=cur>no label yet</div>
<div class=row><button id=m_still class=on onclick="mode('still')">I stand STILL</button><button id=m_walk onclick="mode('walk')">I WALK slowly</button></div>
<h3>Tap the cell you are in (hub = bottom-left; each cell is about 2 ft)</h3><div id=g></div>
<h3>Other</h3><div class=row><button onclick="lab('empty')">EMPTY ROOM (I am out)</button><button onclick="lab('sitting')">sitting</button></div>
<div class=row><input id=f maxlength=24 placeholder=custom style="flex:1;font:inherit;padding:8px"><button onclick="lab(f.value.replace(/[^A-Za-z0-9_-]/g,'_'));f.value=''">label</button></div>
<div id=st></div>
<script>let M='still';const T=location.pathname.split('/')[1];
const g=document.getElementById('g');for(let r=5;r>=1;r--)for(let c=1;c<=5;c++){const b=document.createElement('button');
b.textContent=(r==1&&c==1)?'HUB':'c'+c+'r'+r;b.onclick=()=>lab('c'+c+'r'+r+'_'+M);g.appendChild(b)}
function mode(m){M=m;m_still.className=m=='still'?'on':'';m_walk.className=m=='walk'?'on':''}
function lab(l){if(!l)return;fetch('/'+T+'/label?l='+encodeURIComponent(l),{method:'POST'}).then(r=>r.json()).then(j=>{cur.textContent=j.ok?'NOW: '+l:'FAILED: '+j.err}).catch(()=>cur.textContent='FAILED: Pi unreachable')}
setInterval(()=>fetch('/'+T+'/state').then(r=>r.json()).then(j=>st.textContent='frames '+j.frames+' | labels '+j.labels+' | '+j.fps+' frames/s').catch(()=>{}),2000)</script>"""

def record(out, port):
    """No schedule and no time limit. The hub stops a capture after 20 min (safety), so CAPEND re-arms `csicap on` (a gap of a few seconds)."""
    import serial, os
    subprocess.run(["stty", "-F", port, "115200", "raw", "-echo", "-hupcl"], check=True)
    ser = serial.Serial(port, 115200, timeout=0.2)
    stop = out + ".stop"
    if os.path.exists(stop): os.remove(stop)
    def send(s): ser.write((s + "\n").encode()); ser.flush()
    def arm():
        ser.baudrate = 115200; ser.reset_input_buffer(); send(""); send("csicap on")
        end = time.time() + 5
        while time.time() < end:
            if "csicap on" in ser.readline().decode("utf-8", "replace"): break
        else: sys.exit("hub did not acknowledge `csicap on`")
        time.sleep(0.3); ser.baudrate = 921600; ser.reset_input_buffer()
    n = {"CSI": 0, "LBL": 0}; t0 = time.time(); last = t0
    import threading, secrets, http.server, urllib.parse, socket
    token = secrets.token_hex(4); lock = threading.Lock(); st = {"fps": 0}
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def _send(self, code, body, ct="application/json"):
            b = body.encode(); self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            if u.path == "/" + token + "/state": self._send(200, '{"frames":%d,"labels":%d,"fps":%d}' % (n["CSI"], n["LBL"], st["fps"]))
            elif u.path in ("/" + token, "/" + token + "/"): self._send(200, PAGE, "text/html")
            else: self._send(404, "{}")
        def do_POST(self):
            u = urllib.parse.urlparse(self.path)
            lb = "".join(ch for ch in urllib.parse.parse_qs(u.query).get("l", [""])[0] if ch.isalnum() or ch in "_-")[:24]
            if u.path != "/" + token + "/label" or not lb: return self._send(400, '{"ok":false,"err":"bad label"}')
            with lock: send("csilabel " + lb)
            self._send(200, '{"ok":true}')
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 8099), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
    try: ip = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split()[0]
    except Exception: ip = "<pi-ip>"
    print(f"LABEL PAGE: http://{ip}:8099/{token}/", flush=True)
    fr_mark = (time.time(), 0)
    try:
        arm(); print("RECORDING (label in the dashboard; stop with: touch %s)" % stop, flush=True)
        with open(out, "a") as f:
            while not os.path.exists(stop):
                l = ser.readline().decode("utf-8", "replace").strip(); k = l[:3]
                if k in ("CSI", "LBL", "CAP") and l.count(",") >= 2:
                    f.write(l + "\n")
                    if k in n: n[k] += 1
                    if k == "CAP": f.flush(); arm(); print(f"{time.time()-t0:5.0f}s re-armed after CAPEND", flush=True)
                if time.time() - fr_mark[0] > 2: st["fps"] = (n["CSI"] - fr_mark[1]) / (time.time() - fr_mark[0]); fr_mark = (time.time(), n["CSI"])
                if time.time() - last > 30:
                    f.flush(); last = time.time(); print(f"  {last-t0:5.0f}s frames={n['CSI']} labels={n['LBL']} {st['fps']:.0f}/s", flush=True)
    finally:
        try: srv.shutdown()
        except Exception: pass
        try:
            ser.baudrate = 921600; send("csicap off"); time.sleep(0.5); ser.baudrate = 115200
        except Exception: pass
        print(f"stopped: frames={n['CSI']} labels={n['LBL']} -> {out}", flush=True)

def load(path):
    import numpy as np
    L = []; fr = {}
    for l in open(path):
        p = l.strip().split(",")
        if p[0] == "LBL": L.append((int(p[1]), p[2]))
        elif p[0] == "CSI" and len(p) == 9 and len(p[8]) == 256 and int(p[2]) > 0:
            b = np.frombuffer(bytes.fromhex(p[8]), dtype=np.int8).astype(float)
            fr.setdefault(int(p[2]), []).append((int(p[1]), int(p[4]), np.abs(b[1::2] + 1j * b[0::2])))
    return L, fr

def analyze(path):
    import numpy as np
    L, fr = load(path)
    if not L or not fr: sys.exit("no labelled frames in file")
    use = [k for k in range(1, 64) if not 27 <= k <= 37]
    nodes = sorted(fr)
    t_end = max(f[-1][0] for f in fr.values())
    segs = [(n, s + 8000, (L[i+1][0] if i + 1 < len(L) else t_end) - 2000) for i, (s, n) in enumerate(L)]
    W = {}            # label -> node -> list of (tstd, rssi) per 1 s window (50 frames, 50 % overlap)
    for node in nodes:
        t = np.array([f[0] for f in fr[node]]); r = np.array([f[1] for f in fr[node]], float)
        A = np.array([f[2] for f in fr[node]])[:, use]
        an = A / np.maximum(A.mean(1, keepdims=True), 1e-6)
        for lab, s, e in segs:
            idx = np.where((t >= s) & (t < e))[0]
            for i in range(0, len(idx) - 50 + 1, 25):
                j = idx[i:i+50]
                W.setdefault(lab, {}).setdefault(node, []).append((an[j].std(0).mean(), r[j].mean()))
    print(f"frames per link: { {k: len(v) for k, v in fr.items()} }  (links = hub<->node)")
    E = next((l for l in W if "empty" in l), None)
    if E is None: sys.exit("no label containing 'empty'")
    base = {nd: (np.median([w[0] for w in W[E][nd]]), np.median([w[1] for w in W[E][nd]]),
                 np.percentile([w[0] for w in W[E][nd]], 95)) for nd in nodes if nd in W[E]}
    labs = [l for l in W if "empty" not in l]
    print("\nPer link and label: tstd ratio vs empty median / rssi shift dB / % windows above empty's p95 tstd")
    print(f"{'label':12s}" + "".join(f"   node {nd:<14d}" for nd in base))
    for lab in list(W):
        row = f"{lab:12s}"
        for nd in base:
            w = W[lab].get(nd, [])
            if not w: row += "   -               "; continue
            ts = np.array([x[0] for x in w]); rs = np.array([x[1] for x in w])
            row += f"   {np.median(ts)/base[nd][0]:4.2f} {np.median(rs)-base[nd][1]:+5.1f}dB {100*np.mean(ts>base[nd][2]):3.0f}%"
        print(row)
    still = [l for l in labs if "walk" not in l]
    def vec(lab):
        nds = list(base); n = min(len(W[lab].get(nd, [])) for nd in nds)
        if n == 0: return np.zeros((0, 2 * len(nds)))
        return np.array([[v for nd in nds for v in (np.log(W[lab][nd][i][0] / base[nd][0]), W[lab][nd][i][1] - base[nd][1])] for i in range(n)])
    V = {l: vec(l) for l in still + [E] if l in W}
    tr, te = [], []
    for l, v in V.items():
        h = len(v) // 2
        tr += [(l, x) for x in v[:h]]; te += [(l, x) for x in v[h:]]
    if len(V) > 2 and tr and te:
        X = np.array([x for _, x in tr]); y = [l for l, _ in tr]; sd = X.std(0) + 1e-6
        ok = 0; conf = {}
        for l, x in te:
            d = np.linalg.norm((X - x) / sd, axis=1); k = np.argsort(d)[:5]
            pred = max(set(y[i] for i in k), key=[y[i] for i in k].count); ok += pred == l
            if pred != l: conf[(l, pred)] = conf.get((l, pred), 0) + 1
        print(f"\nSpot classification (still spots + empty, kNN on first half -> second half of each spot): {ok}/{len(te)} = {100*ok/len(te):.0f}%  (chance {100/len(V):.0f}%)")
        for (a, b), c in sorted(conf.items(), key=lambda kv: -kv[1])[:8]: print(f"  {a} mistaken for {b}: {c}")

if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "run": run(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM2")
    elif len(sys.argv) >= 3 and sys.argv[1] == "record": record(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM2")
    elif len(sys.argv) >= 3 and sys.argv[1] == "analyze": analyze(sys.argv[2])
    else: print(__doc__)
