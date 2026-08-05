#!/usr/bin/env python3
"""Badge-side SKU gating test (run against a flashed --test build on COM8).

Enumerates the COMPILED tool categories via the `tool_list` harness command and
asserts the ACTIVE RESEARCH categories are absent on Sn34k-Boy / present on
Res34rch-Boy. Robust to the per-SKU array compaction because it matches by NAME.

Usage:
    py -3 test_sku_gating.py --expect sn34k [--port COM8]
    py -3 test_sku_gating.py --expect res34rch  [--port COM8]
"""
import argparse
import sys
from harness import Harness

# DETECT-LED taxonomy (Jun 2026). PASSIVE cats ship in BOTH SKUs; ACTIVE
# cats are the contiguous research tail gated behind --res34rch.
ACTIVE = {"Deauth", "Flood", "Beacon Spam", "BLE Spam", "SAE", "Evil Portal"}
PASSIVE = {"Detect", "Scan", "Monitor", "Analyze", "Utilities/Lists", "Network"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", required=True, choices=["sn34k", "res34rch"])
    ap.add_argument("--port", default=None)
    args = ap.parse_args()

    h = Harness(port=args.port)
    fails = []
    try:
        r = h.cmd("tool_list")
        assert r.get("ok"), f"tool_list failed: {r}"
        names = {c["name"] for c in r["cats"]}
        count = r["count"]
        print(f"SKU build reports {count} categories: {sorted(names)}")

        present_active = ACTIVE & names
        missing_passive = PASSIVE - names

        if missing_passive:
            fails.append(f"passive categories MISSING: {sorted(missing_passive)}")

        if args.expect == "sn34k":
            if present_active:
                fails.append(f"ACTIVE categories present in Sn34k: {sorted(present_active)}")
            if count != len(PASSIVE):
                fails.append(f"Sn34k category count {count} != {len(PASSIVE)} passive")
            # An out-of-range (active-id) start must be rejected (no tool runs).
            h.cmd("tool_start 12 0")
            h.wait(400)
            st = h.tool_state()
            if st.get("running"):
                fails.append(f"out-of-range tool_start started something: {st.get('name')}")
                h.tool_stop()
        else:  # res34rch
            missing_active = ACTIVE - names
            if missing_active:
                fails.append(f"ACTIVE categories MISSING in Res34rch: {sorted(missing_active)}")
            if count != len(PASSIVE) + len(ACTIVE):
                fails.append(f"Res34rch category count {count} != {len(PASSIVE)+len(ACTIVE)}")
    finally:
        h.close()

    print("=" * 56)
    if fails:
        print(f"FAIL ({args.expect}): {len(fails)} issue(s):")
        for f in fails:
            print("  - " + f)
        return 1
    print(f"PASS ({args.expect}): tool categories match the expected SKU.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
