#!/usr/bin/env python3
"""Continuous timestamped serial logger for hub and probe (debug aid for calibration-v2).

Writes captures/serial/hub.log and probe.log (every line, timestamped) and captures/serial/changes.log
(detector state changes, pairing/link changes, resets/crash markers, re-learn/calibration events).
Ports are the by-path USB slots (hub = usb-0:1.1, probe = usb-0:1.3); never opens with dtr/rts, so boards are not reset.
Usage: serial_logger.py [outdir]   (runs until killed; reconnects on error)
"""
import os, re, subprocess, sys, time, threading

OUT = sys.argv[1] if len(sys.argv) > 1 else "captures/serial"
BY = "/dev/serial/by-path/platform-fd500000.pcie-pci-0000:01:00.0-usb-0:1.%d:1.0"
PORTS = {"hub": BY % 1, "probe": BY % 3}
CRASH = re.compile(r"rst:|task_wdt|Guru|Backtrace|abort\(\)|panic", re.I)
EVT = re.compile(r"re-learn|drift|calibrat|baseline|touch|pair", re.I)
lock = threading.Lock()
os.makedirs(OUT, exist_ok=True)
chg = open(os.path.join(OUT, "changes.log"), "a", buffering=1)


def stamp():
    t = time.time()
    return time.strftime("%F %T", time.localtime(t)) + ".%03d" % (t % 1 * 1000)


def note(name, msg):
    with lock:
        chg.write("%s %-5s %s\n" % (stamp(), name, msg))


def run(name, path):
    log = open(os.path.join(OUT, name + ".log"), "a", buffering=1)
    last = {"pres": None, "paired": None, "rssi": None}
    while True:
        try:
            subprocess.run(["stty", "-F", path, "115200", "raw", "-echo", "-hupcl"], check=True)
            fd = os.open(path, os.O_RDONLY)
            note(name, "logger: opened " + path)
            buf = b""
            while True:
                d = os.read(fd, 4096)
                if not d:
                    raise OSError("eof")
                buf += d
                *lines, buf = buf.split(b"\n")
                for raw in lines:
                    s = raw.decode("utf-8", "replace").rstrip("\r")
                    log.write("%s %s\n" % (stamp(), s))
                    if CRASH.search(s):
                        note(name, "CRASH/RESET: " + s[:200])
                    elif name == "hub":
                        m = re.search(r"rssi=(-?\d+) loss=([\d.]+)% csi=([\d.]+) tstd=([\d.]+) pres=(\d)", s)
                        if m:
                            p = m.group(5)
                            if p != last["pres"]:
                                note(name, "pres %s -> %s  tstd=%s rssi=%s loss=%s%%" % (last["pres"], p, m.group(4), m.group(1), m.group(2)))
                                last["pres"] = p
                            r = int(m.group(1))
                            if last["rssi"] is not None and abs(r - last["rssi"]) >= 2:
                                note(name, "rssi %d -> %d" % (last["rssi"], r))
                            last["rssi"] = r
                        m = re.search(r"paired=(\d)", s)
                        if m and m.group(1) != last["paired"]:
                            note(name, "paired %s -> %s" % (last["paired"], m.group(1)))
                            last["paired"] = m.group(1)
                        if EVT.search(s) and "paired=" not in s:
                            note(name, s[:200])
                    else:
                        m = re.search(r"err=(\d+)", s)
                        if m and int(m.group(1)) > 0:
                            note(name, "tx err: " + s[:160])
                        if re.search(r"sync_age=\d{4,}", s):
                            note(name, "sync_age high: " + s[-60:])
        except Exception as e:
            note(name, "logger: %s, retrying" % e)
            time.sleep(3)


for n, p in PORTS.items():
    threading.Thread(target=run, args=(n, p), daemon=True).start()
while True:
    time.sleep(3600)
