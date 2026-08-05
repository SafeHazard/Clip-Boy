#!/usr/bin/env python3
"""raw_toolstart.py -- capture the RAW serial stream around a tool_start to see
whether the framed response is never printed / printed late / dropped-corrupted,
and how much log chatter floods in. dtr=False, single session (no reset)."""
import sys, time, serial
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
STX = 0x02

s = serial.Serial()
s.port = PORT; s.baudrate = 115200; s.timeout = 0.1; s.write_timeout = 3
s.dtr = False; s.rts = False
s.open()
time.sleep(0.3)

def send(cmd):
    s.reset_input_buffer()
    s.write(bytes([STX]) + cmd.encode() + b"\n"); s.flush()

# make sure it's alive + nav to Tools
send("skip_boot"); time.sleep(0.3); s.read(4096)
send("nav 1 0");   time.sleep(0.5); s.read(4096)

print("=== sending tool_start 1 0; TIMESTAMPING every line for 25s ===")
t0 = time.time()
send("tool_start 1 0")
buf = b""
resp_seen = None
while time.time() - t0 < 25:
    c = s.read(4096)
    if not c:
        continue
    buf += c
    # emit each COMPLETE line with its arrival timestamp
    while b"\n" in buf:
        nl = buf.find(b"\n")
        line = buf[:nl]; buf = buf[nl+1:]
        txt = line.decode("utf-8", "replace").rstrip("\r")
        if not txt.strip():
            continue
        ts = time.time() - t0
        tag = ""
        if bytes([STX]) in line and b'"cmd":"tool_start"' in line.replace(b" ", b""):
            tag = "  <<< tool_start RESPONSE"; resp_seen = ts
        print(f"  [{ts:6.2f}s] {txt[:110]}{tag}")
print(f"\n[summary] tool_start response {'at %.2fs'%resp_seen if resp_seen else 'NEVER SEEN'}")
s.close()
