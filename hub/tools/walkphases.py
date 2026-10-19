#!/usr/bin/env python3
"""Timed phase test against the live hub (Pi on the hub Wi-Fi: sudo nmcli connection up hub-tether ifname wlan0).
  python3 -I hub/tools/walkphases.py OUT.csv [LEAD_S] [PHASE_S]
Phases after the lead-in: empty (leave the room), walk (around the square), still (middle), empty (leave). Logs the hub's fused
score/vector from /ws and the probe-pair states from /api/p2plinks once a second, prints a per-phase summary. The first 6 s of a
phase are left out of the summary (time to get into place). Read-only: changes nothing on the hub."""
import base64, json, os, socket, sys, time, urllib.request

HUB = "192.168.69.1"
PHASES = [("empty", "LEAVE the room"), ("walk", "WALK around inside the square"), ("still", "STAND STILL in the middle"), ("empty2", "LEAVE the room")]
TRIM = 6

def ws_open():
    s = socket.create_connection((HUB, 80), 5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (HUB, key)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf: buf += s.recv(1024)
    s.settimeout(0.3)
    return s, buf.split(b"\r\n\r\n", 1)[1]

def ws_frames(buf):
    out = []
    while len(buf) >= 2:
        ln = buf[1] & 0x7F; off = 2
        if ln == 126:
            if len(buf) < 4: break
            ln = int.from_bytes(buf[2:4], "big"); off = 4
        elif ln == 127:
            if len(buf) < 10: break
            ln = int.from_bytes(buf[2:10], "big"); off = 10
        if len(buf) < off + ln: break
        p, buf = buf[off:off + ln], buf[off + ln:]
        try: out.append(json.loads(p.decode()))
        except Exception: pass
    return out, buf

def links():
    try: return json.loads(urllib.request.urlopen("http://%s/api/p2plinks" % HUB, timeout=2).read())
    except Exception: return None

def main():
    out = sys.argv[1]; lead = float(sys.argv[2]) if len(sys.argv) > 2 else 60; ph_s = float(sys.argv[3]) if len(sys.argv) > 3 else 30
    s, buf = ws_open()
    sched = [("lead", "get ready", lead)] + [(n, t, ph_s) for n, t in PHASES]
    t_end = []; t = time.time()
    for n, txt, d in sched: t_end.append(t + d); t += d
    rows = []; cur = -1; last = {}; nextlink = 0
    f = open(out, "w"); f.write("t_s,phase,score,vector,pairs_state,pairs_active,p2p_score,hub_nodes_on\n")
    t0 = time.time()
    while time.time() < t_end[-1]:
        now = time.time()
        i = next(k for k, e in enumerate(t_end) if now < e)
        if i != cur: cur = i; print("[%3.0fs] %s for %d s: %s" % (now - t0, sched[i][0].upper(), sched[i][2], sched[i][1]), flush=True)
        try:
            d = s.recv(8192)
            if not d: raise OSError("closed")
            buf += d
        except socket.timeout: pass
        fr, buf = ws_frames(buf)
        if fr: last = fr[-1]
        if now >= nextlink:
            nextlink = now + 1.0; pl = links() or {}
            ps = pl.get("state", ""); pa = pl.get("pairs", ""); psc = pl.get("score", "")
            non = sum(1 for n in last.get("nodes", []) if n.get("on")) if last else 0
            row = (round(now - t0, 1), sched[i][0], last.get("score", ""), last.get("vector", ""), ps, pa, psc, non)
            rows.append(row); f.write(",".join(str(x) for x in row) + "\n"); f.flush()
    f.close()
    print("\nphase   secs  alarm%  meanScore  maxScore  vectors")
    for n, _, _ in sched[1:]:
        r = [x for x in rows if x[1] == n]
        t_first = r[0][0] if r else 0; r = [x for x in r if x[0] - t_first >= TRIM]
        if not r: print(n, "no data"); continue
        sc = [float(x[2]) for x in r if x[2] != ""]; al = sum(1 for x in r if x[3] not in ("QUIET", "", None))
        vc = {}
        for x in r: vc[x[3]] = vc.get(x[3], 0) + 1
        print("%-7s %4d  %5.0f%%  %8.1f  %8.0f  %s" % (n, len(r), 100.0 * al / len(r), sum(sc) / max(1, len(sc)), max(sc or [0]), vc))

main()
