#!/usr/bin/env python3
"""Record raw CSI lines from the hub's UART to a file (research aid, see csicap.cpp).
Usage: ~/.platformio-venv/bin/python hub/tools/csi_capture.py OUTFILE [SECONDS] [PORT]
Starts the capture with `csicap on`, switches the host to 921600 baud, writes every line the hub emits (CSI / LBL / CAPEND),
and on exit (time up, Ctrl-C or error) sends `csicap off` and returns the UART to 115200.
Clears HUPCL and never sets DTR/RTS: touching them resets the board."""
import subprocess, sys, time, serial

out = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600; port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM0"
subprocess.run(["stty", "-F", port, "115200", "raw", "-echo", "-hupcl"], check=True)   # keep DTR/RTS as they are: toggling them resets the board
ser = serial.Serial(port, 115200, timeout=0.2)

def send(s): ser.write((s + "\n").encode()); ser.flush()
def wait_for(sub, t):
    end = time.time() + t
    while time.time() < end:
        l = ser.readline().decode("utf-8", "replace")
        if sub in l: return True
    return False

n = {"CSI": 0, "LBL": 0}
try:
    ser.reset_input_buffer(); send("csicap on")
    if not wait_for("csicap on", 5): sys.exit("hub did not acknowledge `csicap on` (is the hub running this firmware?)")
    time.sleep(0.3); ser.baudrate = 921600; ser.reset_input_buffer()
    print(f"recording to {out} for {secs:.0f} s", flush=True)
    t0 = time.time(); last = t0
    with open(out, "w") as f:
        while time.time() - t0 < secs:
            l = ser.readline().decode("utf-8", "replace").strip()
            k = l[:3]
            if k in ("CSI", "LBL", "CAP") and (l.count(",") >= 2):
                f.write(l + "\n")
                if k in n: n[k] += 1
            if time.time() - last > 15: print(f"  {time.time()-t0:5.0f}s frames={n['CSI']} labels={n['LBL']}", flush=True); last = time.time()
finally:
    try:
        send("csicap off"); wait_for("CAPEND", 3); ser.baudrate = 115200; time.sleep(0.3); wait_for("csicap off", 3)
    except Exception: pass
    ser.close(); print(f"done: frames={n['CSI']} labels={n['LBL']}")
