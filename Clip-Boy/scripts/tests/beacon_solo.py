#!/usr/bin/env python3
"""beacon_solo.py — measure Beacon Random in ISOLATION on a freshly booted board
(nothing else run first), to see whether the earlier 2 fps was residual-state or
a genuine floor. Run recover.py + settle BEFORE this for a clean boot."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

def window(h, label, secs_reset=1, settle=2500, win=4000):
    h.fps_reset(); h.wait(settle); h.fps_reset(); h.wait(win)
    f = h.fps()
    print(f"  {label}: cur={f.get('fps')} avg={f.get('fps_avg')} "
          f"min={f.get('fps_min')} samples={f.get('samples')}")
    return f

def main():
    h = Harness(port="COM8")
    try:
        h.nav(1, 0); h.wait(500)
        bspam = h.cat_pos("Beacon Spam")   # Res34rch-only; None on Sn34k
        if bspam is None:
            print("SKIP: 'Beacon Spam' category absent (Sn34k-Boy build, listen-only).")
            return
        print("Idle baseline:")
        window(h, "idle")
        r = h.tool_start(bspam, 0)   # Beacon Spam > Random
        print(f"tool_start(Beacon Spam,0) -> ok={r.get('ok')} name={r.get('name')}")
        print("Beacon Random (solo, clean boot):")
        window(h, "beacon-random")
        # second window to see stability
        window(h, "beacon-random#2")
        h.tool_stop(); h.wait(500)
        ws = h.tool_state()
        print(f"after stop: running={ws.get('running')} name='{ws.get('name')}'")
        print("Recovery:")
        window(h, "post-stop")
    finally:
        h.close()

if __name__ == "__main__":
    main()
