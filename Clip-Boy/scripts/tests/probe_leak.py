#!/usr/bin/env python3
"""probe_leak.py — distinguish a one-time init cost from a per-cycle leak.
Cycle each LiDAR activity on/off repeatedly in ONE session, printing DRAM after
each cycle. Plateau after cycle 1 => one-time init (not a leak). Monotonic
decline => real per-cycle leak (firmware bug)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

def dram(h):
    return h.heap().get("dram", 0)

def cycle(h, label, start, stop, n=5, on_ms=900, off_ms=700):
    print(f"\n{label}: DRAM after each on/off cycle")
    base = dram(h)
    print(f"  baseline       {base:,}")
    for i in range(n):
        start(); h.wait(on_ms)
        stop();  h.wait(off_ms)
        d = dram(h)
        print(f"  cycle {i+1:>2}        {d:,}   (delta from baseline {d-base:+,})")

def main():
    h = Harness(port="COM8")
    try:
        h.nav(1, 0); h.wait(500)
        scan = h.cat_pos("Scan")   # APs (full) lives under Scan in the new taxonomy
        cycle(h, "TOOL (APs) on/off (control)",
              lambda: h.tool_start(scan, 0), lambda: h.tool_stop(), n=4)
        cycle(h, "THEREMIN on/off (leak-fix check)",
              lambda: h.theremin_start(), lambda: h.theremin_stop(), n=5)
        # HR scan: gentle — rapid start/stop is known to exhaust the VL53L5CX.
        # Big settles between cycles; only 3 cycles, on a sensor not pre-churned.
        cycle(h, "HR SCAN on/off (gentle)",
              lambda: h.hr_scan_start(), lambda: h.hr_scan_stop(),
              n=3, on_ms=1500, off_ms=2000)
    finally:
        h.close()

if __name__ == "__main__":
    main()
