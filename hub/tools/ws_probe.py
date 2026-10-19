# Minimal stdlib WebSocket client: connects to ws://192.168.69.1/ws from a given source IP, counts frames for N seconds.
# usage: python3 -I ws_probe.py <src_ip> <seconds>
import socket, sys, time, json, base64, os
src, secs = sys.argv[1], float(sys.argv[2])
s = socket.socket(); s.settimeout(5); s.bind((src, 0)); s.connect(("192.168.69.1", 80))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: 192.168.69.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(1024)
head, buf = buf.split(b"\r\n\r\n", 1)
print("handshake:", head.split(b"\r\n")[0].decode())
frames, sizes, last, t0 = 0, [], None, time.time()
s.settimeout(1.0)
while time.time() - t0 < secs:
    try:
        buf += s.recv(4096)
    except socket.timeout:
        continue
    while len(buf) >= 2:
        ln = buf[1] & 0x7F; off = 2
        if ln == 126:
            if len(buf) < 4: break
            ln = int.from_bytes(buf[2:4], "big"); off = 4
        elif ln == 127:
            if len(buf) < 10: break
            ln = int.from_bytes(buf[2:10], "big"); off = 10
        if len(buf) < off + ln: break
        payload, buf = buf[off:off + ln], buf[off + ln:]
        if (buf and False) or True:
            try: last = json.loads(payload.decode()); frames += 1; sizes.append(ln)
            except Exception: pass
print("frames=%d in %.1fs (%.1f Hz) avg_size=%d" % (frames, secs, frames / secs, sum(sizes) // max(1, len(sizes))))
print("last frame:", json.dumps(last))
