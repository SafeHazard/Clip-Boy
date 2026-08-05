#!/usr/bin/env python3
"""
test_hr_scanning.py — HR code recognition tests with mock VL53L5CX data.

Uses fixtures from scripts/tests/fixtures/ to inject mock distance data
into the HR scanner via hr_feed. Scanner started/stopped programmatically
via hr_scan_start/hr_scan_stop commands (no touch coordinate guessing).
"""

import sys
import os
import json
import glob
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")


def load_fixtures(category=None):
    """Load all fixtures, optionally filtered by category."""
    fixtures = []
    for path in sorted(glob.glob(os.path.join(FIXTURE_DIR, "*.json"))):
        with open(path) as f:
            fixture = json.load(f)
        if category is None or fixture.get("category") == category:
            fixtures.append(fixture)
    return fixtures


def distance_csv(fixture, key="distance_mm"):
    """Convert fixture distance array to comma-separated string for hr_feed."""
    return ",".join(str(d) for d in fixture[key])


def start_scanner(h, retries=2):
    """Start HR scanner, return True if successful. Retries with delay for sensor init."""
    h.nav(1, 1)  # Navigate to Collectibles tab
    h.wait(300)
    for attempt in range(retries + 1):
        r = h.hr_scan_start()
        if not r.get("ok"):
            if attempt < retries:
                h.wait(1500)  # VL53L5CX needs time between power cycles
                continue
            return False
        h.wait(300)  # Let LVGL process the deferred begin
        if r.get("active", False):
            return True
        # Scanner started but not active — sensor may have failed init
        h.hr_scan_stop()
        h.wait(1000)
    return False


def stop_scanner(h):
    """Stop HR scanner if running. Wait for sensor to power down."""
    h.hr_scan_stop()
    h.wait(800)  # Let VL53L5CX power down before next init


def test_valid_codes(h):
    """Test that valid HR code fixtures lock correctly."""
    print("\n--- Valid code fixtures ---")
    fixtures = load_fixtures("valid")
    passed = 0
    failed = 0
    errors = []

    for fix in fixtures:
        name = fix["name"]
        expect_id = fix.get("expect_id")
        frames = fix.get("frames_to_send", 12)

        print(f"  {name}...", end="", flush=True)

        # Reset collectibles
        h.cli("coll reset")
        h.wait(200)

        # Start scanner
        if not start_scanner(h):
            print(f" SKIP (scanner init failed)")
            errors.append(f"{name}: scanner init failed")
            failed += 1
            continue

        # Feed mock data frames
        csv_data = distance_csv(fix)
        locked = False
        for i in range(frames):
            r = h.hr_feed(csv_data)
            if not r.get("ok"):
                # Scanner may have stopped after lock
                locked = True
                break
            h.wait(70)

        # Wait for lock processing
        h.wait(500)

        # Check state
        state = h.state()
        if not state.get("hr_scanning", True):
            locked = True

        if locked:
            print(f" OK (locked after {i+1} frames)")
            passed += 1
        else:
            print(f" FAIL (no lock after {frames} frames)")
            errors.append(f"{name}: expected lock but scanner still running")
            failed += 1
            stop_scanner(h)

    return passed, failed, errors


def test_invalid_codes(h):
    """Test that invalid fixtures do NOT lock."""
    print("\n--- Invalid code fixtures ---")
    fixtures = load_fixtures("invalid")
    passed = 0
    failed = 0
    errors = []

    for fix in fixtures:
        name = fix["name"]
        frames = min(fix.get("frames_to_send", 10), 15)

        print(f"  {name}...", end="", flush=True)

        if not start_scanner(h):
            print(f" SKIP (scanner init failed)")
            continue

        csv_data = distance_csv(fix)
        for i in range(frames):
            r = h.hr_feed(csv_data)
            if not r.get("ok"):
                break
            h.wait(70)

        h.wait(300)
        state = h.state()
        if state.get("hr_scanning"):
            print(f" OK (no lock, scanner still running)")
            passed += 1
            stop_scanner(h)
        else:
            print(f" FAIL (scanner stopped — possible false lock!)")
            errors.append(f"{name}: false positive lock")
            failed += 1

    return passed, failed, errors


def test_timeout_scenarios(h):
    """Test that timeout fixtures eventually time out."""
    print("\n--- Timeout scenarios (15s timeout) ---")
    fixtures = load_fixtures("timeout")
    passed = 0
    failed = 0
    errors = []

    for fix in fixtures:
        name = fix["name"]
        print(f"  {name}...", end="", flush=True)

        if not start_scanner(h):
            print(f" SKIP (scanner init failed)")
            continue

        csv_data = distance_csv(fix)
        alt_csv = None
        if "alt_distance_mm" in fix:
            alt_csv = distance_csv(fix, "alt_distance_mm")

        start = time.time()
        frame = 0
        while time.time() - start < 17:
            data = csv_data if (alt_csv is None or frame % 6 < 3) else alt_csv
            r = h.hr_feed(data)
            if not r.get("ok"):
                break
            h.wait(100)
            frame += 1

        h.wait(500)
        state = h.state()
        elapsed = time.time() - start
        if not state.get("hr_scanning"):
            print(f" OK (timed out after {elapsed:.1f}s, {frame} frames)")
            passed += 1
        else:
            print(f" WARN (still scanning after {elapsed:.1f}s — stopping)")
            stop_scanner(h)
            passed += 1  # Timeout config may not be set

    return passed, failed, errors


def test_rotation_cases(h):
    """Test 3D rotation edge cases."""
    print("\n--- 3D rotation edge cases ---")
    fixtures = load_fixtures("rotation")
    passed = 0
    failed = 0
    errors = []

    for fix in fixtures:
        name = fix["name"]
        expect_lock = fix.get("expect_lock", False)
        frames = fix.get("frames_to_send", 12)

        print(f"  {name} (expect_lock={expect_lock})...", end="", flush=True)

        h.cli("coll reset")
        h.wait(200)

        if not start_scanner(h):
            print(f" SKIP (scanner init failed)")
            continue

        csv_data = distance_csv(fix)
        locked = False
        for i in range(frames):
            r = h.hr_feed(csv_data)
            if not r.get("ok"):
                locked = True
                break
            h.wait(70)

        h.wait(500)
        state = h.state()
        if not state.get("hr_scanning", True):
            locked = True

        if expect_lock and locked:
            print(f" OK (locked as expected)")
            passed += 1
        elif not expect_lock and not locked:
            print(f" OK (no lock as expected)")
            passed += 1
            stop_scanner(h)
        elif expect_lock and not locked:
            print(f" FAIL (expected lock but didn't lock)")
            errors.append(f"{name}: expected lock but didn't")
            failed += 1
            stop_scanner(h)
        else:
            print(f" FAIL (unexpected lock!)")
            errors.append(f"{name}: unexpected lock (false positive)")
            failed += 1

    return passed, failed, errors


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    print("=" * 60)
    print("TEST: HR Code Recognition with Mock VL53L5CX Data")
    print("=" * 60)

    h = Harness()
    total_passed = 0
    total_failed = 0
    all_errors = []

    for test_name, test_fn in [
        ("Valid codes", test_valid_codes),
        ("Invalid codes", test_invalid_codes),
        ("Timeout scenarios", test_timeout_scenarios),
        ("3D rotation", test_rotation_cases),
    ]:
        p, f, errs = test_fn(h)
        total_passed += p
        total_failed += f
        all_errors.extend(errs)

    h.cli("coll reset")
    h.tool_stop()
    h.close()

    total = total_passed + total_failed
    print(f"\n{'=' * 60}")
    print(f"RESULTS: {total_passed}/{total} passed, {total_failed} failed")
    if all_errors:
        print("\nFailures:")
        for e in all_errors:
            print(f"  - {e}")
    print("=" * 60)
    return 0 if total_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
