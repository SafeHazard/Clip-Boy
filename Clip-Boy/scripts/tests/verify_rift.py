#!/usr/bin/env python3
"""verify_rift.py — capture each new --rift theme + apply the new LED presets.
Robust against the rift build's late-created boot/POST overlay: settles, then
retries skip_boot until the overlay clears, and retries theme_set on timeout."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")
THEMES = [(3, "overseer"), (4, "space_badge"), (0, "mojave")]
LED_PRESETS = [(5, "Overseer"), (6, "Space Badge"), (0, "Mojave")]

def ok_cmd(fn, tries=4, wait_ms=1500, h=None):
    """Call a harness cmd, retrying while it returns ok:False (timeout)."""
    for _ in range(tries):
        try:
            r = fn()
            if r.get("ok"):
                return r
        except Exception:
            pass
        if h:
            h.wait(wait_ms)
    return {"ok": False}

def clear_boot(h):
    # POST overlay is created late on rift; skip_boot deletes it. Retry until the
    # state no longer reports a boot overlay (or we run out of tries).
    for _ in range(12):
        h.skip_boot()
        s = h.state()
        if s.get("ok") and not s.get("boot") and not s.get("boot_overlay"):
            return True
        h.wait(700)
    return False

def main():
    os.makedirs(OUT, exist_ok=True)
    h = Harness(port="COM8")
    try:
        print("settling + clearing boot overlay...")
        h.wait(3000)
        cleared = clear_boot(h)
        print(f"  boot cleared: {cleared}  state={h.state()}")

        h.nav(1, 0); h.wait(800)   # ITEMS > Tools
        print("=== THEME screenshots (ITEMS > Tools) ===")
        for idx, name in THEMES:
            r = ok_cmd(lambda: h.theme_set(idx), h=h)
            h.wait(1500)
            clear_boot(h)  # theme rebuild can re-raise nothing, but be safe
            path = os.path.join(OUT, f"theme_{idx}_{name}.bmp")
            ss = ok_cmd(lambda: h.screenshot(path), h=h)
            print(f"  theme {idx} {name}: set={r.get('ok')} shot={ss.get('ok')}")

        print("\n=== LED presets ===")
        for idx, name in LED_PRESETS:
            r = ok_cmd(lambda: h.led_preset(idx), h=h)
            h.wait(800)
            ns = h.neopixel_state()
            print(f"  preset {idx} {name}: ok={r.get('ok')} leds={ns.get('leds')}")
    finally:
        h.close()

if __name__ == "__main__":
    main()
