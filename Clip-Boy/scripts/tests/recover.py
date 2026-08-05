#!/usr/bin/env python3
"""recover.py — hard-reset the wedged board via DTR/RTS pulse, leave it in a
writable run state (DTR asserted), wait for full boot, confirm an STX ping."""
import sys, time, serial
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
STX = 0x02

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.4
ser.write_timeout = 3
ser.dtr = False
ser.rts = False
ser.open()

# Classic ESP reset pulse: RTS->EN low briefly, DTR->BOOT high (run mode).
ser.setDTR(True)            # BOOT high = normal boot, also DTE-present for CDC
ser.setRTS(True);  time.sleep(0.12)   # EN low (reset asserted)
ser.setRTS(False)           # release EN -> boot
ser.reset_input_buffer()

print("Reset pulsed; waiting for boot banner...")
t0 = time.time(); banner = b""
while time.time() - t0 < 8.0:
    c = ser.read(4096)
    if c:
        banner += c
        sys.stdout.write(c.decode("utf-8", errors="replace")); sys.stdout.flush()
print(f"\n[banner {len(banner)}B]  ClipBoy ready marker: "
      f"{b'ClipBoy ready' in banner or b'Main screen' in banner}")

# Give the blocking ClipBoy init a moment, then ping.
time.sleep(2)
ok = False
for i in range(5):
    try:
        ser.reset_input_buffer()
        ser.write(bytes([STX]) + b"ping\n"); ser.flush()
        t0 = time.time(); resp = b""
        while time.time() - t0 < 2.0:
            c = ser.read(256)
            if c: resp += c
            if b"\x02" in resp and b"\n" in resp: break
        if b'"ok":true' in resp.replace(b" ", b"").lower():
            print(f"ping {i}: OK -> {resp.strip()!r}")
            ok = True; break
        print(f"ping {i}: {resp[:120]!r}")
    except Exception as e:
        print(f"ping {i}: {e!r}")
    time.sleep(1)
ser.close()
print("RECOVERED" if ok else "STILL WEDGED")
sys.exit(0 if ok else 1)
