#!/usr/bin/env python3
"""raw_ble_first.py — capture TIMESTAMPED raw serial while starting a BLE tool as the
FIRST BLE tool. Shows exactly where the ACK stall lives.

Usage: raw_ble_first.py [PORT] ["<cat> <item>"]
Env: CB_NOREBOOT=1 skips the serial 'reboot' (use when the badge was just
     esptool-reset and is already freshly booted)."""
import sys, os, time, serial
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
CATITEM = sys.argv[2] if len(sys.argv) > 2 else "9 5"
NOREBOOT = bool(os.environ.get("CB_NOREBOOT"))
STX = 0x02

s = serial.Serial(); s.port=PORT; s.baudrate=115200; s.timeout=0.1; s.write_timeout=2
s.dtr=False; s.rts=False
for _ in range(10):
    try: s.open(); break
    except Exception: time.sleep(1)
time.sleep(0.5)

def send(c, tries=15):
    """Write a command, retrying through the post-reset CDC-not-ready window."""
    for _ in range(tries):
        try:
            s.reset_input_buffer(); s.write(bytes([STX])+c.encode()+b"\n"); s.flush(); return True
        except serial.SerialTimeoutException:
            time.sleep(1)
    return False

if not NOREBOOT:
    print("reboot + wait for boot...")
    send("reboot"); t0=time.time()
    while time.time()-t0 < 20: s.read(4096)
else:
    print("no-reboot: draining boot banner...")
    t0=time.time()
    while time.time()-t0 < 6: s.read(4096)

if not send("skip_boot"): print("[FATAL] CDC never accepted a write (wedged)"); sys.exit(1)
time.sleep(0.4); s.read(4096)
send("nav 1 0"); time.sleep(0.5); s.read(4096)

print(f"=== tool_start {CATITEM} (first BLE); timestamping 50s ===")
t0=time.time(); send(f"tool_start {CATITEM}")
buf=b""; ack=None; nlines=0
while time.time()-t0 < 50:
    c=s.read(4096)
    if not c: continue
    buf+=c
    while b"\n" in buf:
        nl=buf.find(b"\n"); line=buf[:nl]; buf=buf[nl+1:]
        txt=line.decode("utf-8","replace").rstrip("\r")
        if not txt.strip(): continue
        ts=time.time()-t0; nlines+=1
        tag=""
        if bytes([STX]) in line and b'"cmd":"tool_start"' in line.replace(b" ",b""):
            tag="  <<< tool_start ACK"; ack=ts
        # print CBBLE markers ALWAYS + first 40 lines + ACK
        if txt.startswith("CBBLE") or nlines<=40 or tag:
            print(f"  [{ts:6.2f}s] {txt[:100]}{tag}")
print(f"\n[summary] {nlines} lines in 50s; tool_start ACK {'at %.2fs'%ack if ack else 'NEVER (still blocked)'}")
s.close()
