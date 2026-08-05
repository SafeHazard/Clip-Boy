#!/usr/bin/env python3
"""therm_then_hr.py <count> — single trial on a fresh boot: do <count> theremin
on/off cycles, then ONE HR scan, and report OK or WEDGE. Caller resets the board
between trials so each is independent (no cross-trial accumulation)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

def responsive(h, tries=5, wait_ms=1500):
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
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    h = Harness(port="COM8")
    try:
        h.nav(1, 0); h.wait(500)
        for _ in range(count):
            h.theremin_start(); h.wait(1500); h.theremin_stop(); h.wait(1300)
        if responsive(h, 3) is None:
            print(f"RESULT count={count}: WEDGE during theremin cycling")
            return
        h.hr_scan_start(); h.wait(2000)
        s = responsive(h)
        if s is None:
            print(f"RESULT count={count}: WEDGE on HR scan after {count} theremin cycles")
        else:
            print(f"RESULT count={count}: OK (hr_scanning={s.get('hr_scanning')})")
            h.hr_scan_stop()
    finally:
        h.close()

if __name__ == "__main__":
    main()
