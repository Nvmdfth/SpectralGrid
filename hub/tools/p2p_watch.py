#!/usr/bin/env python3
"""Log every link's CSI noise (tstd) and RSSI once a second: hub<->probe links from the hub's status lines, probe<->probe links
from the probes' PKT_DIAG replies (needs `pcmd all p2p 1`). Reads the hub console only; never sets DTR/RTS (that resets the board).
  python3 hub/tools/p2p_watch.py record OUT.csv SECONDS [PORT]
  python3 hub/tools/p2p_watch.py serve OUT.csv [PORT]            open-ended recording + a label page on the Pi (phone on the home Wi-Fi); stop: touch OUT.csv.stop
  python3 hub/tools/p2p_watch.py grid OUT.csv                     score position accuracy from a labelled grid test
  python3 hub/tools/p2p_watch.py summary OUT.csv [BASELINE_SECONDS]      baseline = first N s (room empty), default 25
Columns: t_s,link,tstd,rssi   link = "h-3" for hub<->node 3, "1>3" for what node 1 hears of node 3."""
import re, subprocess, sys, time

def record(out, secs, port, serve=False):
    import serial, select, os, threading, queue, secrets, http.server, urllib.parse
    subprocess.run(["stty", "-F", port, "115200", "raw", "-echo", "-hupcl"], check=True)
    ser = serial.Serial(port, 115200, timeout=0.2)
    t0 = time.time(); last_cmd = 0; buf = ""; n = 0
    labq = queue.Queue(); cur = {"label": "", "t": 0.0}; srv = None
    if serve:
        token = secrets.token_hex(4); stop = out + ".stop"
        if os.path.exists(stop): os.remove(stop)
        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a): pass
            def _send(self, code, body, ct="application/json"):
                b = body.encode(); self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
            def do_GET(self):
                u = urllib.parse.urlparse(self.path)
                if u.path in ("/" + token, "/" + token + "/"): self._send(200, PAGE.replace("@T@", token), "text/html")
                elif u.path == "/" + token + "/state": self._send(200, '{"label":"%s","age":%.0f,"n":%d}' % (cur["label"], time.time() - cur["t"] if cur["label"] else 0, n))
                else: self._send(404, "{}")
            def do_POST(self):
                u = urllib.parse.urlparse(self.path)
                lb = "".join(ch for ch in urllib.parse.parse_qs(u.query).get("l", [""])[0] if ch.isalnum() or ch in "_-")[:24]
                if u.path != "/" + token + "/label" or not lb: return self._send(400, '{"ok":false}')
                labq.put(lb); cur["label"] = lb; cur["t"] = time.time(); self._send(200, '{"ok":true}')
        srv = http.server.ThreadingHTTPServer(("0.0.0.0", 8099), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
        try: ip = subprocess.run(["ip", "-4", "-o", "addr", "show", "eth0"], capture_output=True, text=True).stdout.split()[3].split("/")[0]
        except Exception: ip = "<pi-ip>"
        print(f"LABEL PAGE (phone on the home Wi-Fi): http://{ip}:8099/{token}/", flush=True)
    done = (lambda: os.path.exists(stop)) if serve else (lambda: time.time() - t0 >= secs)
    with open(out, "a" if serve else "w") as f:
        if f.tell() == 0: f.write("t_s,link,tstd,rssi\n")
        while not done():
            while not labq.empty(): f.write(f"{time.time() - t0:.1f},L,{labq.get()},0\n"); f.flush()
            if time.time() - last_cmd >= 1.0:
                last_cmd = time.time(); ser.write(b"\npdiag\n"); ser.flush()
            try: buf += ser.read(4096).decode("utf-8", "replace")
            except Exception:                       # another reader touched the port (hubctl): reopen and carry on
                time.sleep(0.5)
                try: ser.close(); ser = serial.Serial(port, 115200, timeout=0.2)
                except Exception: pass
            while "\n" in buf:
                l, buf = buf.split("\n", 1); t = time.time() - t0
                m = re.match(r"\s*node (\d+) \S+ .*rssi=(-?\d+) .*tstd=(-?[\d.]+)", l)
                if m and float(m.group(3)) >= 0: f.write(f"{t:.1f},h-{m.group(1)},{m.group(3)},{m.group(2)}\n"); n += 1
                m = re.match(r"diag n(\d+) age=([\d.]+)s .*peers:(.*)", l)
                if m and float(m.group(2)) <= 3.0:      # the hub replays a probe's last report forever: a silent probe must not look alive
                    for p in re.finditer(r"\[n(\d+) fps=(\d+) rssi=(-?[\d.]+) tstd=([\d.]+) win=(\d+)\]", m.group(3)):
                        if int(p.group(2)) > 0: f.write(f"{t:.1f},{m.group(1)}>{p.group(1)},{p.group(4)},{p.group(3)}\n"); n += 1
    if srv: srv.shutdown()
    print(f"recorded {n} samples -> {out}")

PAGE = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1"><title>Grid labels</title>
<style>body{font:16px system-ui;margin:12px;background:#111;color:#eee;max-width:440px;margin:auto;padding:12px}
button{font:inherit;padding:10px;border-radius:6px;border:1px solid #777;background:#222;color:#eee}button.on{background:#0a6;border-color:#0f9}
#g{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin:8px 0}#g button{aspect-ratio:1;font-size:13px}
.row{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0}#cur{font-size:20px;min-height:1.4em}#tm{font-size:34px;font-weight:600}
.ok{color:#3f6}.wait{color:#fb4}.cor{position:absolute;font-size:11px;color:#9ad}</style>
<div id=cur>no label yet</div><div id=tm></div>
<div class=row><button id=ms class=on onclick="mode('still')">STILL</button><button id=mw onclick="mode('walk')">WALK in the cell</button></div>
<div style="position:relative;padding:14px 4px 14px">
<div style="display:flex;justify-content:space-between;font-size:12px;color:#9ad"><span>node 1</span><span>node 4</span></div>
<div id=g></div>
<div style="display:flex;justify-content:space-between;font-size:12px;color:#9ad"><span>HUB</span><span>node 3</span></div></div>
<div class=row><button onclick="lab('empty')">EMPTY ROOM (I am out)</button></div>
<div class=row><input id=f maxlength=24 placeholder="custom label" style="flex:1;font:inherit;padding:8px"><button onclick="lab(f.value.replace(/[^A-Za-z0-9_-]/g,'_'));f.value=''">label</button></div>
<div id=st style="color:#999;font-size:13px"></div>
<script>let M='still';const T='@T@';
const g=document.getElementById('g');for(let r=3;r>=1;r--)for(let c=1;c<=3;c++){const b=document.createElement('button');b.textContent='c'+c+'r'+r;b.onclick=()=>lab('c'+c+'r'+r+'_'+M);g.appendChild(b)}
function mode(m){M=m;ms.className=m=='still'?'on':'';mw.className=m=='walk'?'on':''}
let LT=0;function lab(l){if(!l)return;if(LT&&Date.now()-LT<20000&&!confirm('Only '+Math.round((Date.now()-LT)/1000)+' s on the last label (want 22+). Switch anyway?'))return;LT=Date.now();fetch('/'+T+'/label?l='+encodeURIComponent(l),{method:'POST'}).then(r=>r.json()).then(j=>{cur.textContent=j.ok?'NOW: '+l:'FAILED'}).catch(()=>cur.textContent='FAILED: Pi unreachable')}
setInterval(()=>fetch('/'+T+'/state').then(r=>r.json()).then(j=>{if(j.label)cur.textContent='NOW: '+j.label;tm.textContent=j.label?j.age+' s':'';tm.className=j.age>=22?'ok':'wait';st.textContent='samples '+j.n+(j.age>=22?'  - enough, move on':'  - stay put')}).catch(()=>{}),1000)</script>"""

def summary(path, base_s):
    import statistics as st
    rows = [l.strip().split(",") for l in open(path).read().splitlines()[1:]]
    links = sorted({r[1] for r in rows})
    print(f"baseline = first {base_s:.0f} s; 'peak' = highest 5-sample rolling median after it; 'above' = % of samples after it over the baseline's max")
    print(f"{'link':6s} {'n':>4s} {'base med':>9s} {'base max':>9s} {'peak':>7s} {'ratio':>6s} {'above':>6s} {'rssi shift':>10s}")
    for k in links:
        s = [(float(r[0]), float(r[2]), float(r[3])) for r in rows if r[1] == k]
        b = [x for x in s if x[0] < base_s]; a = [x for x in s if x[0] >= base_s]
        if len(b) < 5 or len(a) < 5: continue
        bm = st.median(x[1] for x in b); bx = max(x[1] for x in b)
        roll = [st.median(x[1] for x in a[i:i + 5]) for i in range(len(a) - 4)]
        pk = max(roll); ab = 100.0 * sum(1 for x in a if x[1] > bx) / len(a)
        rs = st.median(x[2] for x in a) - st.median(x[2] for x in b)
        print(f"{k:6s} {len(s):4d} {bm:9.3f} {bx:9.3f} {pk:7.3f} {pk / bm:6.2f} {ab:5.0f}% {rs:+9.1f}dB")

def grid(path):
    import statistics as st, math
    rows = [l.strip().split(",") for l in open(path).read().splitlines()[1:]]
    labs = [(float(r[0]), r[2]) for r in rows if r[1] == "L"]
    data = {}
    for r in rows:
        if r[1] != "L": data.setdefault(r[1], []).append((float(r[0]), float(r[2])))
    links = sorted(data)
    if not labs or not links: sys.exit("need labels and link data")
    T = max(r[0] for k in links for r in data[k])
    segs = []                                   # (label, occurrence, t0, t1)
    occ = {}
    for i, (t, lb) in enumerate(labs):
        e = labs[i + 1][0] if i + 1 < len(labs) else T
        occ[lb] = occ.get(lb, 0) + 1; segs.append((lb, occ[lb], t + 8, e - 1))
    def series(k, a, b): return [v for t, v in data[k] if a <= t < b]
    base = {}
    for lb, o, a, b in segs:
        if "empty" in lb:
            for k in links: base.setdefault(k, []).extend(series(k, a, b))
    if not base: sys.exit("no 'empty' segment")
    base = {k: st.median(v) for k, v in base.items() if v}
    def feat(a, b):                              # per-second windows of log-ratio vectors
        out = []
        t = a
        while t + 4 <= b:
            v = [series(k, t, t + 4) for k in links]
            if all(v): out.append([math.log(st.median(x) / base[k]) for x, k in zip(v, links)])
            t += 2
        return out
    F = {(lb, o): feat(a, b) for lb, o, a, b in segs if b - a >= 6}
    print("links:", links); 
    print(f"{'segment':16s} {'n':>3s}  median log-ratio per link")
    for (lb, o), f in F.items():
        if f: print(f"{lb + '#' + str(o):16s} {len(f):3d}  " + " ".join(f"{st.median(c):+5.2f}" for c in zip(*f)))
    def cell(lb):
        m = lb.split("_")[0]
        if len(m) == 4 and m[0] == "c" and m[2] == "r": return (int(m[1]), int(m[3]))
    def ctr(c): return ((c[0] - 0.5) * 10 / 3, (c[1] - 0.5) * 10 / 3)   # feet from the hub corner
    for mode in ("still", "walk"):
        keys = [k for k in F if k[0].endswith("_" + mode) and cell(k[0]) and F[k]]
        cells = sorted({cell(k[0]) for k in keys})
        by = {c: [k for k in keys if cell(k[0]) == c] for c in cells}
        multi = [c for c in cells if len(by[c]) >= 2]
        if len(multi) >= 3:       # train on the first visit of each cell, test on later visits
            tr = {c: [x for k in by[c][:1] for x in F[k]] for c in multi}; te = [(c, x) for c in multi for k in by[c][1:] for x in F[k]]
            how = "first visit -> later visits"
        else:                     # one visit only: first half -> second half
            cs = cells; tr = {c: [x for k in by[c] for x in F[k][: len(F[k]) // 2]] for c in cs}; te = [(c, x) for c in cs for k in by[c] for x in F[k][len(F[k]) // 2:]]
            how = "first half -> second half of each visit"
        tr = {c: v for c, v in tr.items() if v}
        if len(tr) < 3 or not te: print(f"\n{mode}: not enough labelled cells yet ({len(tr)})"); continue
        cen = {c: [st.mean(col) for col in zip(*v)] for c, v in tr.items()}
        sd = [max(st.pstdev([x for v in tr.values() for x in [y[i] for y in v]]), 0.05) for i in range(len(links))]
        ok = near = 0; errs = []
        for c, x in te:
            pred = min(cen, key=lambda q: sum(((a - b) / s) ** 2 for a, b, s in zip(x, cen[q], sd)))
            ok += pred == c; near += max(abs(pred[0] - c[0]), abs(pred[1] - c[1])) <= 1
            (px, py), (cx, cy) = ctr(pred), ctr(c); errs.append(math.hypot(px - cx, py - cy))
        n = len(te)
        print(f"\n{mode} ({how}; {len(tr)} cells, chance {100/len(tr):.0f}%): exact cell {100*ok/n:.0f}%, within one cell {100*near/n:.0f}%, mean position error {st.mean(errs):.1f} ft, median {st.median(errs):.1f} ft  ({n} windows)")

if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "record": record(sys.argv[2], float(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else "/dev/ttyACM2")
    elif len(sys.argv) >= 3 and sys.argv[1] == "serve": record(sys.argv[2], 0, sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM2", serve=True)
    elif len(sys.argv) >= 3 and sys.argv[1] == "grid": grid(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "summary": summary(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 25)
    else: print(__doc__)
