#!/usr/bin/env python3
"""
test_modal_heap_loop.py -- Q6 crash-class tests for the HR scanner (DC34), REAL-tag.

Uses the REAL sensor path only (no hr_feed mock injection -- the mock path has a
separate CV-pipeline bug tracked elsewhere, and real frames are what ships). You hold
a physical OmniTag in front of the sensor; the test drives real scan->lock->dismiss
cycles and watches heap.

  PART A -- MODAL HEAP LOOP (hold a valid tag)
    N x (scan_start -> wait for a REAL lock -> hr_dismiss), asserting the free-heap
    LOW-WATER (min_dram, monotonic-down) stays FLAT. A leaking "found" modal would
    stair-step it down. `coll remove` runs each cycle so it's always the HEAVY
    "newly unlocked" modal (art card) -- worst case.

  PART B -- SCANNER CHURN / TIMEOUT (remove the tag)
    Rapid scan_start->scan_stop teardown x M, then a few REAL timeouts (scan with no
    tag in view -> natural ~15s timeout), asserting NO REBOOT (session drop) + flat heap.

Requires the --test build (TEST_HARNESS). Persistent harness session (no DTR churn).

Usage:
  py -3 scripts/tests/test_modal_heap_loop.py [lock_id] [modal_iters] [churn_iters]
  (defaults: id 15, 30 modal, 40 churn)  -- id 15/73/118/124 lock fast & clean.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from harness import Harness

LOCK_ID = int(sys.argv[1]) if len(sys.argv) > 1 else 15
LEAK_KB_TOL = 8
LOCK_WAIT_S = 8.0        # per-cycle: how long to wait for a real lock


def min_dram(h):
    try:
        return int(h.heap().get("min_dram", 0))
    except Exception:
        return None


def alive(h):
    try:
        st = h.state()
    except Exception:
        return False
    return isinstance(st, dict) and ("hr_scanning" in st or "heap" in st or "cmd" in st)


def countdown(msg, secs=6):
    print("\n>>> %s" % msg)
    for s in range(secs, 0, -1):
        print("    starting in %d ..." % s, end="\r", flush=True)
        time.sleep(1)
    print("    GO                      ")


def start_scanner(h, retries=2):
    h.nav(1, 1); h.wait(250)
    for _ in range(retries + 1):
        r = h.hr_scan_start(); h.wait(250)
        if r.get("active"):
            return True
        h.hr_scan_stop(); h.wait(1200)
    return False


def wait_real_lock(h, timeout_s):
    """Poll state until the scanner stops (a real decode locked it) or timeout."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        st = h.state()
        if not st:
            return "dead"
        if not st.get("hr_scanning", True):
            return "lock"
        h.wait(150)
    return "nolock"


def part_a_modal_loop(h, iters):
    print("\n=== PART A: modal heap loop (%d x REAL scan->lock->dismiss, id %d) ===" % (iters, LOCK_ID))
    countdown("HOLD the OmniTag (pattern %d) ~3in from the sensor and keep it there" % LOCK_ID, 8)
    if not alive(h):
        print("  badge unresponsive at start"); return False, None

    base = min_dram(h)
    print("  baseline min_dram = %s KB" % (base // 1024 if base else "?"))
    lows = [base] if base else []
    locks = misses = 0
    for i in range(iters):
        if not alive(h):
            print("  iter %d: BADGE UNRESPONSIVE (reboot?) -- ABORT" % (i + 1)); return False, lows
        h.cli("coll remove %d" % LOCK_ID); h.wait(80)   # force heavy fresh-unlock modal
        if not start_scanner(h):
            misses += 1; continue
        res = wait_real_lock(h, LOCK_WAIT_S)
        if res == "dead":
            print("  iter %d: BADGE UNRESPONSIVE -- ABORT" % (i + 1)); return False, lows
        if res == "lock":
            locks += 1
            # Let the success modal + lazy PSRAM image finish building BEFORE dismissing.
            # hr_dismiss -> hr_scan_finish_modal calls rebuild_content(); firing it mid-build
            # is a rebuild-during-transition UAF surface (this is what rebooted the badge when
            # the harness dismissed too aggressively). 700ms is comfortably past the image copy.
            h.wait(700)
        else:
            misses += 1; h.hr_scan_stop(); h.wait(250)
        h.cmd("hr_dismiss"); h.wait(200)
        md = min_dram(h)
        if md:
            lows.append(md)
        if (i + 1) % 5 == 0:
            drop = (base - min(lows)) // 1024 if base and lows else 0
            print("  %3d/%d  locks=%d misses=%d  min_dram=%sKB (drop %dKB)"
                  % (i + 1, iters, locks, misses, md // 1024 if md else 0, drop))
    drop_kb = (base - min(lows)) // 1024 if base and lows else 0
    ok = drop_kb <= LEAK_KB_TOL and locks >= max(3, iters * 0.4)
    print("  -> locks=%d/%d  min_dram drop=%dKB (tol %dKB)  %s"
          % (locks, iters, drop_kb, LEAK_KB_TOL, "PASS" if ok else "FAIL (few locks? re-aim tag)"))
    return ok, lows


def part_b_churn(h, iters):
    print("\n=== PART B: scanner churn/timeout (%d x start->stop + 3 real timeouts) ===" % iters)
    countdown("REMOVE the tag (clear the sensor's view) for the timeout phase", 6)
    base = min_dram(h)
    print("  baseline min_dram = %s KB" % (base // 1024 if base else "?"))
    lows = [base] if base else []
    for i in range(iters):
        if not alive(h):
            print("  churn %d: UNRESPONSIVE (reboot?) -- ABORT" % (i + 1)); return False
        h.nav(1, 1); h.wait(120)
        h.hr_scan_start(); h.wait(180)
        h.hr_scan_stop(); h.wait(280)
        md = min_dram(h)
        if md:
            lows.append(md)
        if (i + 1) % 10 == 0:
            print("  churn %3d/%d  min_dram=%sKB" % (i + 1, iters, md // 1024 if md else 0))

    print("  3 real timeouts (scan with no tag in view -> natural timeout)...")
    for t in range(3):
        if not start_scanner(h):
            print("    timeout %d: scanner wouldn't start" % (t + 1)); continue
        res = wait_real_lock(h, 18.0)   # expect 'nolock' (times out) or 'dead'
        if res == "dead":
            print("    timeout %d: BADGE UNRESPONSIVE -- FAIL" % (t + 1)); return False
        st = h.state()
        stopped = not st.get("hr_scanning", True)
        print("    timeout %d: %s, alive=%s" % (t + 1, "timed out" if stopped else "still scanning", alive(h)))
        if not stopped:
            h.hr_scan_stop(); h.wait(400)

    drop_kb = (base - min(lows)) // 1024 if base and lows else 0
    ok = drop_kb <= LEAK_KB_TOL and alive(h)
    print("  -> min_dram drop=%dKB (tol %dKB)  alive=%s  %s"
          % (drop_kb, LEAK_KB_TOL, alive(h), "PASS" if ok else "FAIL"))
    return ok


def main():
    modal_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    churn_iters = int(sys.argv[3]) if len(sys.argv) > 3 else 40

    print("=" * 62)
    print("HR SCANNER CRASH-CLASS TESTS (REAL-tag modal loop + scanner churn)")
    print("  lock_id=%d  modal_iters=%d  churn_iters=%d" % (LOCK_ID, modal_iters, churn_iters))
    print("=" * 62)
    h = Harness()
    try:
        a_ok, _ = part_a_modal_loop(h, modal_iters)
        b_ok = part_b_churn(h, churn_iters)
    finally:
        try:
            h.hr_scan_stop(); h.cli("coll reset"); h.tool_stop()
        except Exception:
            pass
        h.close()

    print("\n" + "=" * 62)
    print("RESULT: modal-loop %s | scanner-churn %s"
          % ("PASS" if a_ok else "FAIL", "PASS" if b_ok else "FAIL"))
    print("=" * 62)
    return 0 if (a_ok and b_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
