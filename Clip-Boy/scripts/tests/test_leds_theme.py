#!/usr/bin/env python3
"""
test_leds_theme.py — exercise the theme_set / led_set / led_preset harness
commands, verify they take effect on the strip, and restore original state.

Asserts against `neopixel_state` which reads straight off the NeoPixel
strip buffer, so we see the real pixel values (brightness-scaled).
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness


# Brightness scaling: neo_apply does r * (brightness/255)
def scaled(v, brightness):
    return int(v * brightness / 255)


def read_strip(h):
    r = h.cmd("neopixel_state")
    if not r.get("ok"):
        raise RuntimeError(f"neopixel_state failed: {r}")
    return r["leds"]


def test_led_set_static(h):
    """Set each LED to a known static color, verify strip matches."""
    print("\n--- led_set (static colors, anim=0) ---")
    # Set LEDs 0-7 to distinct colors at 50% brightness, anim=None
    targets = [
        (255, 0,   0),    # red
        (0,   255, 0),    # green
        (0,   0,   255),  # blue
        (255, 255, 0),    # yellow
        (255, 0,   255),  # magenta
        (0,   255, 255),  # cyan
        (255, 128, 0),    # orange
        (128, 128, 128),  # gray
    ]
    bright = 128
    for idx, (r, g, b) in enumerate(targets):
        rr = h.led_set(idx, r, g, b, brightness=bright, anim=0)
        if not rr.get("ok"):
            return False, [f"led_set {idx} failed: {rr.get('error', '?')}"]

    # Give the NeoPixel task one animation tick to flush
    h.wait(200)

    strip = read_strip(h)
    errors = []
    for i, (r, g, b) in enumerate(targets):
        got = strip[i]
        want_r, want_g, want_b = scaled(r, bright), scaled(g, bright), scaled(b, bright)
        if (got["r"], got["g"], got["b"]) != (want_r, want_g, want_b):
            errors.append(f"LED {i}: wrote ({r},{g},{b})@{bright}, expected ({want_r},{want_g},{want_b}), got ({got['r']},{got['g']},{got['b']})")
        else:
            print(f"  LED {i}: ({got['r']},{got['g']},{got['b']}) OK")
    return (len(errors) == 0), errors


def test_led_preset_off(h):
    """Preset 4 = Off. All LEDs should go to (0,0,0)."""
    print("\n--- led_preset 4 (Off) ---")
    r = h.led_preset(4)
    if not r.get("ok"):
        return False, [f"led_preset failed: {r.get('error', '?')}"]
    h.wait(200)
    strip = read_strip(h)
    for i, px in enumerate(strip):
        if (px["r"], px["g"], px["b"]) != (0, 0, 0):
            return False, [f"LED {i} not off after preset Off: got ({px['r']},{px['g']},{px['b']})"]
    print(f"  All {len(strip)} LEDs at (0,0,0)")
    return True, []


def test_led_preset_mojave(h):
    """Preset 0 = Mojave (amber chase). All LEDs use amber base color, but
    chase animation alternates amber/black. Just verify SOMETHING is amber."""
    print("\n--- led_preset 0 (Mojave amber chase) ---")
    r = h.led_preset(0)
    if not r.get("ok"):
        return False, [f"led_preset failed: {r.get('error', '?')}"]
    h.wait(300)  # let chase tick
    strip = read_strip(h)
    # At least one LED should be amber-ish (r > 100, g between 20-200, b very low)
    amber_count = sum(1 for px in strip if px["r"] > 100 and 20 < px["g"] < 200 and px["b"] < 20)
    if amber_count == 0:
        return False, [f"No amber-ish LEDs seen after Mojave preset: {strip}"]
    print(f"  {amber_count}/{len(strip)} LEDs show amber tones (chase animation active)")
    return True, []


def test_theme_set(h):
    """Switch themes live, verify cfg.theme persists, restore original."""
    print("\n--- theme_set (live switch) ---")
    r0 = h.cfg_get("theme")
    original = r0["value"]
    print(f"  Original theme: {original}")

    # Pick a different theme
    target = (original + 1) % 3
    r = h.theme_set(target)
    if not r.get("ok"):
        return False, [f"theme_set failed: {r.get('error', '?')}"]
    if not r.get("changed"):
        return False, [f"theme_set reported no change when switching {original}->{target}"]
    print(f"  Switched to theme {target}")

    # Let UI rebuild
    h.wait(500)

    # Verify cfg.theme updated
    check = h.cfg_get("theme")
    if check["value"] != target:
        return False, [f"cfg.theme not updated: got {check['value']}, want {target}"]
    print(f"  cfg.theme = {target} confirmed")

    # Verify the badge is still responsive (no crash from rebuild)
    state = h.state()
    if not state.get("ok"):
        return False, ["Badge unresponsive after theme switch"]
    print(f"  Badge responsive: div={state['div']} tab={state['tab']}")

    # Restore original
    h.theme_set(original)
    h.wait(500)
    return True, []


def main():
    print("=" * 60)
    print("TEST: LEDs + Theme (harness commands)")
    print("=" * 60)

    h = Harness()

    # Save original LED config so we can restore at the end
    original_strip = read_strip(h)

    tests = [
        ("theme_set live",    test_theme_set),
        ("led_set static",    test_led_set_static),
        ("led_preset Off",    test_led_preset_off),
        ("led_preset Mojave", test_led_preset_mojave),
    ]

    passed = 0
    failed = 0
    all_errors = []
    for name, fn in tests:
        try:
            ok, errs = fn(h)
            if ok:
                passed += 1
            else:
                failed += 1
                all_errors.extend([f"{name}: {e}" for e in errs])
        except Exception as e:
            print(f"  CRASH: {e}")
            failed += 1
            all_errors.append(f"{name}: CRASH — {e}")

    # Restore — apply Mojave preset (the default)
    print("\nRestoring Mojave preset (default)...")
    h.led_preset(0)
    h.wait(200)

    h.close()

    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"RESULTS: {passed}/{total} passed, {failed} failed")
    if all_errors:
        print("\nFailures:")
        for e in all_errors:
            print(f"  - {e}")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
