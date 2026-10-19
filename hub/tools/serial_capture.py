# usage: python -I serial_capture.py <out.log> <seconds> [cmd_file]
# Captures the board's UART without a second open. NOTE: opening the port RESETS the board (the CH343 adapter's DTR/RTS
# drive EN/BOOT), and so does every other process that opens it. To send a console command while capturing, write it to
# cmd_file (default /tmp/serial_cmd): this process writes it to the already-open port and deletes the file.
import serial, time, sys, os
out_path, secs = sys.argv[1], float(sys.argv[2])
cmd_path = sys.argv[3] if len(sys.argv) > 3 else "/tmp/serial_cmd"
s = serial.Serial(); s.port = '/dev/ttyACM0'; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False; s.open()
t = time.time(); out = open(out_path, 'wb')
while time.time() - t < secs:
    d = s.read(512)
    if d: out.write(d); out.flush()
    if os.path.exists(cmd_path):
        try:
            cmd = open(cmd_path).read().strip(); os.remove(cmd_path)
            if cmd: s.write((cmd + "\n").encode()); s.flush(); out.write(("\n[capture] sent: " + cmd + "\n").encode()); out.flush()
        except OSError: pass
