#!/usr/bin/env python3
"""hr_repeat.py — the scanner is the core collectibles mechanic; it must survive
heavy repeated use. Cycle HR scan start/stop many times on a clean boot and find
whether/when it wedges. Reports the cycle count at first wedge (or survival).

Usage: hr_repeat.py [N cycles] [on_ms] [off_ms]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

def responsive(h, tries=4, wait_ms=1500):
    for _ in range(tries):
        try:
            s = h.tool_state()
            if s.get("ok"):
                return s
        except Exception:
            pass
        h.wait(wait_ms)
    return None

def main():
    n      = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    on_ms  = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    off_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
    h = Harness(port="COM8")
    try:
        h.nav(1, 0); h.wait(500)
        print(f"HR scan start/stop x{n}  (on {on_ms}ms, off {off_ms}ms):")
        for i in range(1, n + 1):
            r = h.hr_scan_start()
            if not r.get("ok"):
                print(f"  cycle {i:>2}: START unresponsive")
                s = responsive(h)
                if s is None:
                    print(f"  *** WEDGE at cycle {i} (start) ***"); return
            h.wait(on_ms)
            s = responsive(h, tries=3)
            if s is None:
                print(f"  *** WEDGE at cycle {i} (after start) ***"); return
            scanning = s.get("hr_scanning")
            h.hr_scan_stop()
            h.wait(off_ms)
            s2 = responsive(h, tries=3)
            if s2 is None:
                print(f"  *** WEDGE at cycle {i} (after stop) ***"); return
            free = s2.get("ok") and h.heap().get("dram", 0)
            print(f"  cycle {i:>2}: ok (scanning={scanning}) DRAM={free:,}")
        print(f"SURVIVED all {n} cycles")
    finally:
        h.close()

if __name__ == "__main__":
    main()
