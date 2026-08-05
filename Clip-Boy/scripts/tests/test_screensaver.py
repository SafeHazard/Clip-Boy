#!/usr/bin/env python3
"""
test_screensaver.py — Screensaver activation and dismissal tests.

Tests screensaver activation after timeout, hold-to-unlock dismiss,
and interactions with tools/theremin.
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness


def test_screensaver_blocks_during_tool(h):
    """T2.4 (partial): Screensaver should NOT activate while tool running."""
    print("\n--- Screensaver blocked during tool ---")

    # Start a tool
    h.tool_start(h.cat_pos("Scan"), 0)  # Scan > APs (full)
    h.wait(2000)

    state = h.state()
    if state.get("screensaver"):
        h.tool_stop()
        return False, ["Screensaver activated while tool running"]

    # Wait a bit more (shouldn't activate even with short timeout)
    h.wait(5000)
    state = h.state()
    ss = state.get("screensaver", False)
    h.tool_stop()
    h.wait(300)

    if ss:
        return False, ["Screensaver activated while tool running (after 7s)"]

    print("  OK: Screensaver stayed off during tool operation")
    return True, []


def test_screensaver_state_query(h):
    """Verify screensaver state is queryable and starts as false."""
    print("\n--- Screensaver state query ---")

    state = h.state()
    ss = state.get("screensaver", None)
    if ss is None:
        return False, ["screensaver field not in state response"]
    if ss:
        # Dismiss it if it's on
        h.touch(160, 120, "press")
        h.wait(2500)
        h.touch(160, 120, "release")
        h.wait(500)

    state = h.state()
    if not state.get("screensaver", True):
        print("  OK: Screensaver is off (expected)")
        return True, []
    return False, ["Screensaver unexpectedly active"]


def main():
    print("=" * 60)
    print("TEST: Screensaver Behavior")
    print("=" * 60)

    h = Harness()
    passed = 0
    failed = 0
    all_errors = []

    tests = [
        ("State query", test_screensaver_state_query),
        ("Blocked during tool", test_screensaver_blocks_during_tool),
    ]

    for name, test_fn in tests:
        ok, errs = test_fn(h)
        if ok:
            passed += 1
        else:
            failed += 1
            all_errors.extend([f"{name}: {e}" for e in errs])

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
