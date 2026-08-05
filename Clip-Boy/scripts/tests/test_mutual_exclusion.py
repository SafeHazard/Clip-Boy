#!/usr/bin/env python3
"""
test_mutual_exclusion.py — when the user starts tool B, tool A must reliably
shut down first, for EVERY tool, with NO wedge (customer fix-action must never
be "reboot the badge").

Two layers of coverage:

  LAYER 1 — Cross-subsystem matrix (the dangerous part). The four activity
  CLASSES that hold a hardware resource — radio tool, geiger (radio), HR scan
  (LiDAR/VL53L5CX), theremin (LiDAR/VL53L5CX + I2S) — are run pairwise A->B for
  every ordered pair (12 transitions). After starting B we assert: B is active,
  A is NOT, no stray activity, the board still RESPONDS (no wedge), and heap is
  stable. The teardown in firmware is keyed on the start path (activity class),
  not the specific tool, so one tool ("APs (full)") represents the whole
  radio-tool family here — the torn-down code path is identical for all.

  LAYER 2 — Every individual tool. Each TAT_SIMPLE tool is exercised both as
  pre-emptor (started while a baseline tool runs -> baseline must be replaced,
  exactly one tool left) and as pre-empted (a baseline started over it -> it
  must be gone). This proves every tool in the catalog starts over and is torn
  down cleanly, single-tool, board responsive.

Authorized targets only. Brief start/stop of active-TX tools (beacon/BLE spam)
is the same exposure as test_tool_sequence. LiDAR (HR scan + theremin) starts
are kept modest and spaced to avoid VL53L5CX exhaustion.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness
from test_tool_sequence import TESTABLE_TOOLS, resolve_tools

# Activity classes and how to read "is this class active?" from tool_state.
CLASS_ACTIVE = {
    "tool":   lambda s: bool(s.get("running")),
    "geiger":   lambda s: bool(s.get("geiger_active")),
    "hrscan":   lambda s: bool(s.get("hr_scanning")),
    "theremin": lambda s: bool(s.get("theremin_active")),
}
CLASSES = ["tool", "geiger", "hrscan", "theremin"]

# APs (full) — passive, represents the radio-tool class. Its category ("Scan")
# position is resolved per-session into TOOL_REP by main() (robust to reorg/SKU).
TOOL_REP = None   # set to (scan_pos, 0) in main()


def start_class(h, kind):
    if kind == "tool":   return h.tool_start(*TOOL_REP)
    if kind == "geiger":   return h.geiger_start()
    if kind == "hrscan":   return h.hr_scan_start()
    if kind == "theremin": return h.theremin_start()
    raise ValueError(kind)


def stop_all(h):
    """Best-effort tear down of everything, leaving a clean slate."""
    h.tool_stop()      # stops tool + geiger + mp3
    h.hr_scan_stop()
    h.theremin_stop()
    h.wait(400)


def state_responsive(h, tries=5, wait_ms=1500):
    """Return (state_dict, responsive_bool). A *single* slow response is not a
    wedge: heavy radio transitions (e.g. first BLE-spam activation) can block
    the firmware loop past the 10s session timeout for one command and then
    recover. Only a board that stays unresponsive across several retries is
    truly wedged. Retry tool_state before giving up."""
    last = {}
    for i in range(tries):
        try:
            s = h.tool_state()
            if s.get("ok"):
                return s, True
            last = s
        except Exception as e:
            last = {"_err": str(e)}
        h.wait(wait_ms)
    return last, False


def active_classes(s):
    return [k for k in CLASSES if CLASS_ACTIVE[k](s)]


def heap_dram(h):
    try:
        return h.heap().get("dram", 0)
    except Exception:
        return 0


# ───────────────────────── LAYER 1: cross-subsystem matrix ──────────────────

def layer1_matrix(h):
    print("\n" + "=" * 60)
    print("LAYER 1: cross-subsystem A->B matrix (tool/geiger/HR/theremin)")
    print("=" * 60)
    passed = failed = 0
    errors = []
    dram0 = heap_dram(h)

    for a in CLASSES:
        for b in CLASSES:
            if a == b:
                continue
            label = f"{a:8} -> {b:8}"
            stop_all(h)

            # Start A, confirm it came up.
            ra = start_class(h, a)
            h.wait(1500 if a in ("hrscan", "theremin") else 1000)
            sa, ok_a = state_responsive(h)
            if not ok_a:
                print(f"  {label}: SETUP FAIL — board not responsive after starting {a}")
                failed += 1; errors.append(f"{label}: unresponsive after A={a}")
                continue
            if not CLASS_ACTIVE[a](sa):
                # A didn't start (e.g. sensor failed) — report, skip the pair.
                print(f"  {label}: SKIP — {a} did not start (active={active_classes(sa)}, ra={ra})")
                continue

            # Start B — the transition under test.
            rb = start_class(h, b)
            h.wait(1500 if b in ("hrscan", "theremin") else 1000)
            sb, ok_b = state_responsive(h)

            if not ok_b:
                # The worst outcome: the transition WEDGED the board.
                print(f"  {label}: *** WEDGE *** board unresponsive after starting {b} "
                      f"(rb={rb}, err={sb.get('_err')})")
                failed += 1; errors.append(f"{label}: WEDGE")
                return passed, failed, errors, True  # abort; board needs reset

            act = active_classes(sb)
            issues = []
            if not CLASS_ACTIVE[b](sb):
                issues.append(f"B({b}) not active")
            if CLASS_ACTIVE[a](sb):
                issues.append(f"A({a}) STILL ACTIVE (not shut down)")
            stray = [c for c in act if c not in (b,)]
            if stray:
                issues.append(f"stray active: {stray}")

            if issues:
                print(f"  {label}: FAIL — {'; '.join(issues)} (active={act})")
                failed += 1; errors.append(f"{label}: {'; '.join(issues)}")
            else:
                print(f"  {label}: ok (only {b} active, board responsive)")
                passed += 1

    stop_all(h)
    dram1 = heap_dram(h)
    leak = dram0 - dram1
    print(f"\n  DRAM: start={dram0:,} end={dram1:,} delta={leak:,}")
    if leak > 8192:
        errors.append(f"DRAM leak across matrix: {leak} bytes")
        failed += 1
    return passed, failed, errors, False


# ───────────────────────── LAYER 2: every individual tool ───────────────────

def layer2_every_tool(h):
    print("\n" + "=" * 60)
    print("LAYER 2: every TAT_SIMPLE tool as pre-emptor AND pre-empted")
    print("=" * 60)
    passed = failed = 0
    errors = []

    base_cat, base_item = TOOL_REP
    stop_all(h)
    rbase = h.tool_start(base_cat, base_item)
    base_name = rbase.get("name", "APs (full)")
    h.wait(800)

    # Resolve catalog category NAMES -> live array positions (drops SKU-absent
    # cats, e.g. the active-research family on Sn34k-Boy).
    for cat, item, name, wtype, infra, note, heavy in resolve_tools(h, TESTABLE_TOOLS):
        if (cat, item) == TOOL_REP:
            continue
        label = f"{name} (cat{cat}/i{item})"

        # --- as PRE-EMPTOR: baseline running, start this tool, baseline must go.
        # (baseline is already running from the prior iteration's tail or setup)
        s, ok = state_responsive(h)
        if not ok:
            print(f"  {label}: board unresponsive before test"); failed += 1
            errors.append(f"{label}: unresponsive pre"); return passed, failed, errors, True
        if not s.get("running"):
            h.tool_start(base_cat, base_item); h.wait(600)

        r = h.tool_start(cat, item)
        h.wait(900)
        s, ok = state_responsive(h)
        if not ok:
            print(f"  {label}: *** WEDGE *** after start"); failed += 1
            errors.append(f"{label}: WEDGE"); return passed, failed, errors, True

        act = active_classes(s)
        issues = []
        if not s.get("running"):
            issues.append("not running after start")
        # Compare firmware-vs-firmware only. tool_start's ACK and tool_state BOTH report
        # cb_op_name, so a real "old op not replaced" shows two different FIRMWARE names.
        # When the ACK is lost (documented for BLE Spam > "! All" -- see memory
        # ble_spam_all_first_hang; test_tool_sequence carves it out too), r has no "name"
        # and the old `r.get("name", name)` fell back to this script's OWN label, so an
        # unrelated slow-ACK tool reported a bogus name mismatch. Say "ACK lost" instead.
        if "name" not in r:
            print(f"  {label}: note - tool_start ACK lost/slow; judging by tool_state only")
        elif s.get("name", "") != r.get("name"):
            issues.append(f"name='{s.get('name')}' != started '{r.get('name')}' (old op not replaced?)")
        stray = [c for c in act if c != "tool"]
        if stray:
            issues.append(f"stray non-tool active: {stray}")

        # --- as PRE-EMPTED: start baseline back over it; this tool must be gone.
        rb = h.tool_start(base_cat, base_item)
        h.wait(700)
        s2, ok2 = state_responsive(h)
        if not ok2:
            print(f"  {label}: *** WEDGE *** after baseline re-start"); failed += 1
            errors.append(f"{label}: WEDGE-2"); return passed, failed, errors, True
        if s2.get("name", "") != base_name:
            issues.append(f"after baseline restart name='{s2.get('name')}' (tool not torn down?)")
        if len(active_classes(s2)) != 1:
            issues.append(f"not single-activity after restore: {active_classes(s2)}")

        if issues:
            print(f"  {label}: FAIL — {'; '.join(issues)}")
            failed += 1; errors.append(f"{label}: {'; '.join(issues)}")
        else:
            print(f"  {label}: ok")
            passed += 1

    stop_all(h)
    return passed, failed, errors, False


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", choices=["1", "2", "both"], default="both")
    # Port was hardcoded to COM8, so this suite could only ever run against ONE
    # bench badge. Match the rest of the suite: explicit --port > CLIPBOY_PORT env
    # > bridge auto-detect (Harness applies that precedence when port is None).
    ap.add_argument("--port", default=None, help="Serial port (default: $CLIPBOY_PORT)")
    args = ap.parse_args()

    print("=" * 60)
    print("TEST: Tool Mutual Exclusion (start B => A shuts down; no wedge)")
    print("=" * 60)

    h = Harness(port=args.port)
    total_p = total_f = 0
    all_errors = []
    aborted = False

    # Resolve the radio-tool representative (APs (full), under "Scan") to its
    # live array position once per session — robust to the menu reorg/SKU.
    global TOOL_REP
    TOOL_REP = (h.cat_pos("Scan"), 0)

    try:
        # Warmup: exercise the radio once so the first heavy BLE-spam activation
        # isn't also paying NimBLE cold-start (known to stall early in uptime).
        h.tool_start(*TOOL_REP); h.wait(1500); h.tool_stop(); h.wait(1200)

        if args.layer in ("1", "both"):
            p, f, errs, abort = layer1_matrix(h)
            total_p += p; total_f += f; all_errors += [f"L1 {e}" for e in errs]
            aborted = abort
        if args.layer in ("2", "both") and not aborted:
            p, f, errs, abort = layer2_every_tool(h)
            total_p += p; total_f += f; all_errors += [f"L2 {e}" for e in errs]
            aborted = abort
    finally:
        try:
            stop_all(h)
        except Exception:
            pass
        h.close()

    total = total_p + total_f
    print("\n" + "=" * 60)
    print(f"RESULTS: {total_p}/{total} passed, {total_f} failed"
          + ("  *** ABORTED (board wedged — needs reset) ***" if aborted else ""))
    if all_errors:
        print("\nFailures:")
        for e in all_errors:
            print(f"  - {e}")
    print("=" * 60)
    return 0 if (total_f == 0 and not aborted) else 1


if __name__ == "__main__":
    sys.exit(main())
