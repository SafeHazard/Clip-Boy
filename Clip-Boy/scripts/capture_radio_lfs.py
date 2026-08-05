#!/usr/bin/env python3
"""Trigger the littlefs radio-stream perf test over serial and capture stats.

Opens the badge serial port with DTR/RTS deasserted (avoids the USB-CDC
reset-on-open gotcha), sends `radiolfs` (seed-if-needed + stream from littlefs),
and logs every [LFS]/[PCM] line for a window, then `radiostop`.

Usage: py -3 scripts/capture_radio_lfs.py [COMxx] [seconds]
"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.dtr = False          # set before open so the board isn't reset
s.rts = False
s.timeout = 0.3
s.open()
time.sleep(1.5)
s.reset_input_buffer()

s.write(b"radiolfs\n")
s.flush()
print(f"[host] sent 'radiolfs' on {port}; capturing {secs:.0f}s ...\n")

t0 = time.time()
keep = ("[LFS]", "[PCM]", "seed", "stream", "open")
while time.time() - t0 < secs:
    line = s.readline().decode("utf-8", "replace").strip()
    if line and any(k in line for k in keep):
        print(f"  {line}")

s.write(b"radiostop\n")
s.flush()
time.sleep(0.5)
for _ in range(20):
    line = s.readline().decode("utf-8", "replace").strip()
    if line and any(k in line for k in keep):
        print(f"  {line}")
s.close()
print("\n[host] done.")
