#!/usr/bin/env python3
"""therm_hr_cyclic.py — repeatedly alternate theremin and HR scan in one session,
stressing the bus handoff both directions every round. After each step assert the
expected single activity is live and the board is responsive (no wedge). Used to
confirm the --rift boot variant behaves identically to stock past boot.

Usage: therm_hr_cyclic.py [rounds]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

def st(h, tries=4, wait_ms=1500):
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
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    h = Harness(port="COM8")
    bad = 0
    try:
        h.nav(1, 0); h.wait(500)
        for r in range(1, rounds + 1):
            # theremin on
            h.theremin_start(); h.wait(1500)
            s = st(h)
            if s is None: print(f"  round {r}: *** WEDGE on theremin start ***"); bad += 1; break
            t_ok = s.get("theremin_active") and not s.get("hr_scanning")
            # handoff theremin -> HR (the fixed path)
            h.hr_scan_start(); h.wait(2000)
            s = st(h)
            if s is None: print(f"  round {r}: *** WEDGE on theremin->HR handoff ***"); bad += 1; break
            h_ok = s.get("hr_scanning") and not s.get("theremin_active")
            # HR off (back to idle); next round's theremin_start tests HR->theremin
            h.hr_scan_stop(); h.wait(1200)
            s = st(h)
            if s is None: print(f"  round {r}: *** WEDGE after HR stop ***"); bad += 1; break
            idle_ok = not s.get("hr_scanning") and not s.get("theremin_active")
            flags = ("OK" if (t_ok and h_ok and idle_ok) else
                     f"FLAG-MISMATCH t={t_ok} h={h_ok} idle={idle_ok}")
            if not (t_ok and h_ok and idle_ok): bad += 1
            print(f"  round {r:>2}: {flags}  DRAM={h.heap().get('dram',0):,}")
        print(f"\n{'ALL ROUNDS CLEAN' if bad == 0 else str(bad)+' round(s) had issues'} ({rounds} rounds)")
    finally:
        h.close()
    sys.exit(0 if bad == 0 else 1)

if __name__ == "__main__":
    main()
