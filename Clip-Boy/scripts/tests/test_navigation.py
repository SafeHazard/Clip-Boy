#!/usr/bin/env python3
"""
test_navigation.py — Test all 9 division/tab navigation combinations.

Uses session mode (persistent serial connection) to avoid DTR resets.
Verifies navigation state, FPS stability, and memory leaks.
"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

DIVISIONS = ["STATS", "ITEMS", "DATA"]
TABS = [
    ["Status", "L.E.E.T.", "Radiation"],
    ["Tools", "Collectibles", "SAOs"],
    ["LEDs", "Settings", "Theremin"],
]

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    passed = 0
    failed = 0
    errors = []

    print("=" * 60)
    print("TEST: Navigation — all 9 screens + stress cycling")
    print("=" * 60)

    h = Harness()

    # Initial state
    print("\n[1] Baseline...")
    heap_start = h.heap()
    dram_start = heap_start.get("dram", 0)
    fps_start = h.fps()
    print(f"  DRAM: {dram_start:,}  FPS: {fps_start.get('fps', 0)}")

    # T1.1 — All 9 screens load
    print("\n[2] Visiting all 9 screens...")
    h.fps_reset()
    for div in range(3):
        for tab in range(3):
            test_name = f"nav_{div}_{tab}"
            r = h.nav(div, tab)
            if not r.get("ok"):
                print(f"  FAIL: nav {div} {tab} failed: {r}")
                failed += 1
                errors.append(f"nav {div} {tab}: command failed")
                continue

            h.wait(200)
            try:
                h.assert_state(div=div, tab=tab,
                               div_name=DIVISIONS[div],
                               tab_name=TABS[div][tab])
                print(f"  OK: {DIVISIONS[div]} > {TABS[div][tab]}")
                passed += 1
            except AssertionError as e:
                print(f"  FAIL: {e}")
                failed += 1
                errors.append(str(e))

            h.screenshot(os.path.join(OUTPUT_DIR, f"{test_name}.bin"))

    # T1.2 — Round-trip
    print("\n[3] Round-trip test...")
    h.nav(0, 0)
    h.wait(100)
    try:
        h.assert_state(div=0, tab=0, div_name="STATS", tab_name="Status")
        print("  OK: Back to STATS > Status")
        passed += 1
    except AssertionError as e:
        print(f"  FAIL: {e}")
        failed += 1
        errors.append(str(e))

    # T1.3 — FPS check after navigation
    print("\n[4] FPS check after 9 screens...")
    fps_after = h.fps()
    fps_val = fps_after.get("fps", 0)
    fps_min = fps_after.get("fps_min", 0)
    print(f"  FPS current: {fps_val}  min: {fps_min}")
    if fps_min < 15 and fps_min > 0:
        print(f"  WARNING: FPS dropped to {fps_min} during navigation")
        errors.append(f"FPS min {fps_min} below 15")

    # T1.4 — Rapid cycling (10 full loops)
    print("\n[5] Rapid cycling (10x all 9 screens)...")
    h.fps_reset()
    for cycle in range(10):
        for div in range(3):
            for tab in range(3):
                h.nav(div, tab)
        if cycle % 5 == 4:
            heap = h.heap()
            print(f"  Cycle {cycle+1}: DRAM={heap.get('dram',0):,}")

    fps_rapid = h.fps()
    print(f"  FPS after 90 navigations: current={fps_rapid.get('fps',0)} min={fps_rapid.get('fps_min',0)}")
    if fps_rapid.get("fps_min", 99) < 15:
        print(f"  WARNING: FPS dropped to {fps_rapid['fps_min']} during rapid cycling")
        errors.append(f"Rapid cycling FPS min {fps_rapid['fps_min']}")

    # Memory leak check
    heap_end = h.heap()
    dram_end = heap_end.get("dram", 0)
    dram_diff = dram_start - dram_end
    print(f"\n[6] Memory: DRAM start={dram_start:,} end={dram_end:,} delta={dram_diff:+,}")
    if dram_diff > 4096:
        print(f"  WARNING: Possible DRAM leak ({dram_diff} bytes)")
        errors.append(f"DRAM leak: {dram_diff} bytes")

    # LED rate check (should be stable regardless of nav)
    led = h.led_rate()
    print(f"  LED rate: {led.get('rate', 0)}/sec")

    h.close()

    # Summary
    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"RESULTS: {passed}/{total} passed, {failed} failed")
    if errors:
        print("\nIssues:")
        for e in errors:
            print(f"  - {e}")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
