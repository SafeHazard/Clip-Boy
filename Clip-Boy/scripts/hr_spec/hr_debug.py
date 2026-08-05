#!/usr/bin/env py -3
"""Trigger + capture the firmware's HR debug dump (DC34-155 scan diagnosis).

Start a scan on the badge and PRESENT the tag, then (while still holding it, so
the last processed frame has the tag in view) run:

    py -3 scripts/hr_spec/hr_debug.py [COM10]

Prints the sampled 16-cell bits, decode status/id/crc, threshold, per-cell
votes, and the 8x8 depth view for the most recent frame. The dump is raw
multi-line (not STX-framed), so this reads it directly rather than via the
bridge. Needs the --test firmware with the hr_debug harness command.
"""
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
s = serial.Serial()
s.port = port; s.baudrate = 115200
s.dtr = False; s.rts = False          # don't reset the badge on open
s.timeout = 0.3
s.open(); time.sleep(0.4); s.reset_input_buffer()
s.write(b"\x02hr_debug\n"); s.flush()
time.sleep(1.3)
out = s.read(30000).decode("utf-8", "replace")
s.close()

# Trim to the dump section if the markers are present.
b = out.find("--- HR_DEBUG_BEGIN ---")
e = out.find("--- HR_DEBUG_END ---")
print(out[b:e + 21] if (b >= 0 and e > b) else out)
