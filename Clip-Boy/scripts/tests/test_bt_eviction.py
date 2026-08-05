#!/usr/bin/env py -3
"""test_bt_eviction.py -- prove the BT device list EVICTS THE OLDEST when full.

FB10 gave `bt_devices` a dedup check and a cap. `test_teardown_paths.py::FB10` proves the
dedup half (count <= NimBLE's 50-distinct-address ceiling). This proves the OTHER half, which
nothing covered: that a full list makes room for a new arrival instead of dropping it.

WHY THE COUNT CANNOT ANSWER THIS
Once the list is full, evict-oldest and drop-new both pin the size at exactly the cap, forever.
A size reading is therefore identical under the fix and under the bug -- the observable cannot
read positive. The discriminator has to be MEMBERSHIP:

  * evict-oldest -> a MAC that was NOT in the first full sample appears in a later one, so the
    UNION of MACs observed across samples grows past the cap.
  * drop-new     -> once full, no new address is ever inserted, so the union stays at exactly
    the membership of the first full sample.

Nothing clears the list mid-scan, so union > cap is only reachable by an insertion into a full
list -- i.e. by an eviction. (BLE private-address rotation helps rather than hurts: a rotated
address is a new MAC to the badge, so it still requires an insertion.)

WHY THIS NEEDS A SPECIAL BUILD -- AND WHAT IT DOES *NOT* PROVE
⚠ CORRECTED 2026-07-27. This test exercises the evict-oldest branch, and on a SHIPPING build that
branch is UNREACHABLE -- not merely hard to reach. NimBLE's `setMaxResults(50)` (set in
WiFiScan::RunBluetoothScan) is enforced BEFORE our callback, its policy is drop-new, and neither
live feeder of `bt_devices` (BT_SCAN_ALL, BT_SCAN_SKIMMERS) disables the duplicate filter or
appears in WiFiScan::main()'s clear-and-restart list. So the badge is never told about a 51st
distinct address and `size() >= CB_BT_LIST_CAP` is never tested against a new insert, at ANY cap
value. `CB_BT_LIST_CAP` is 50 (was 250 -- a number nothing could reach either).

A green run here therefore proves the eviction CODE is correct; it does NOT describe shipped
behaviour, which is first-50-per-run drop-new. Do not cite this test as evidence about what a
user sees. Build the badge with a small cap first:

  CB_EXTRA_DEFS='-DCB_BT_LIST_CAP=4' bash scripts/build.sh --test
  bash scripts/reliable_flash.sh COM4
  py -3 scripts/tests/test_bt_eviction.py --port COM4 --cap 4

That build prints "*** NOT a shippable build ***". RE-FLASH THE SHIPPING BUILD AFTERWARDS.

The cap is passed in rather than read from the firmware ON PURPOSE: the precondition is then
checked against OBSERVED behaviour (does the list actually plateau at this size?) instead of
against a constant the badge merely claims, so a stale or mis-set #define shows up as a
cannot-test rather than as a pass.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness            # noqa: E402

FAILED = []
PASSED = []


def record(name, ok, msg):
    (PASSED if ok else FAILED).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")


def cannot_test(name, why):
    print(f"  [warn] {name}: cannot test: {why}")
    CANNOT.append(name)


CANNOT = []


def sample(h):
    """One membership reading -> (size, set_of_macs) or (None, None) if unreadable."""
    r = h.cmd("bt_list") or {}
    if "devices" not in r:
        return None, None
    macs = {d.get("mac", "").lower() for d in r["devices"] if d.get("mac")}
    return r.get("count"), macs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--cap", type=int, required=True,
                    help="the CB_BT_LIST_CAP this firmware was built with (e.g. 4)")
    ap.add_argument("--secs", type=int, default=150, help="observation window")
    a = ap.parse_args()

    print("=" * 68)
    print(f"TEST: BT list evicts the oldest entry when full (cap={a.cap})")
    print("=" * 68)
    if a.cap > 12:
        print(f"  refusing: cap {a.cap} exceeds bt_list's 12-entry dump limit, so membership "
              f"cannot be read completely and a 'frozen' verdict would be an artefact")
        return 2

    h = Harness(port=a.port)
    try:
        # bt_list must exist, or every reading below is a silent None.
        probe = h.cmd("bt_list")
        if not probe or "devices" not in probe:
            cannot_test("BT-EVICT", "this build has no `bt_list` harness command "
                                    "(rebuild with --test)")
            return summarise()

        sp = h.cat_pos("Scan")
        if sp is None:
            cannot_test("BT-EVICT", "Scan category not found")
            return summarise()

        h.tool_stop()
        h.wait(800)
        h.tool_start(sp, 3)                    # Scan > BT Devices
        h.wait(2500)
        if not (h.tool_state() or {}).get("running"):
            cannot_test("BT-EVICT", "the BT scan would not start")
            return summarise()

        union = set()
        first_full = None
        samples = 0
        removed_seen = False
        deadline = time.time() + a.secs
        while time.time() < deadline:
            time.sleep(5)
            size, macs = sample(h)
            if size is None:
                continue
            samples += 1
            union |= macs
            if first_full is None and size >= a.cap:
                first_full = set(macs)
                print(f"  list reached the cap at sample {samples}: {sorted(first_full)}")
            elif first_full is not None:
                gone = first_full - macs
                if gone and not removed_seen:
                    removed_seen = True
                    print(f"  an entry from the first full sample is gone: {sorted(gone)}")
            if samples % 3 == 0:
                print(f"  +{samples * 5:3d}s size={size} union={len(union)}")
            # Enough evidence to conclude early.
            if first_full is not None and len(union) > a.cap:
                break

        print(f"  samples={samples} distinct MACs observed={len(union)} cap={a.cap}")

        # ── preconditions, each of which makes a verdict meaningless if unmet ──────────
        if samples == 0:
            cannot_test("BT-EVICT", "no readable bt_list sample in the whole window")
            return summarise()
        if first_full is None:
            cannot_test("BT-EVICT",
                        f"the list never reached {a.cap} entries, so the full-list path was "
                        f"never exercised. Either this build does not have "
                        f"-DCB_BT_LIST_CAP={a.cap}, or fewer than {a.cap} BLE devices are in "
                        f"range. A verdict here would be about an un-run code path")
            return summarise()
        if len(union) <= a.cap:
            # POSITIVE CONTROL: the room must contain MORE distinct devices than the cap, or
            # eviction is never triggered and "membership froze" proves nothing.
            cannot_test("BT-EVICT",
                        f"only {len(union)} distinct BLE addresses were ever seen, which is not "
                        f"more than the cap ({a.cap}) -- nothing ever needed to be evicted, so "
                        f"a frozen membership is the CORRECT result and cannot be told apart "
                        f"from drop-new. Lower the cap or test somewhere busier")
            return summarise()

        # ── verdict ───────────────────────────────────────────────────────────────────
        record("BT-EVICT", True,
               f"{len(union)} distinct addresses passed through a {a.cap}-entry list, so an "
               f"arrival was inserted AFTER the list was full -- eviction works"
               + (" (and an original entry was observed leaving)" if removed_seen else ""))
        return summarise()
    finally:
        try:
            h.tool_stop()
        except Exception:                       # noqa: BLE001
            pass


def summarise():
    print("=" * 68)
    for n in CANNOT:
        print(f"  warn: {n} -- NOT RUN (see reason above); this is not a pass")
    print(f"passed={len(PASSED)} failed={len(FAILED)} not-run={len(CANNOT)}")
    print("=" * 68)
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
