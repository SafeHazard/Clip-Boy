#!/usr/bin/env python3
"""probe_toolcycle.py -- test the "resource accumulation over tool cycles" hypothesis.

Cycle ONE Scan tool start/stop repeatedly in a SINGLE session (harness dtr=False =>
no reboot between cycles, exactly like run_all.py's per-module subprocesses leave the
badge). After each cycle record: free DRAM, PSRAM, and the wall-clock time for a
`tool_start` and a trivial `state` query. Then reboot and re-baseline.

Reads:
  MONOTONIC dram decline + growing tool_start/state time => real per-cycle leak/degrade.
  Plateau after cycle 1                                   => one-time init (NOT a leak).
  Post-reboot baseline back to fast/high-heap             => confirms reboot-reversible.

Usage: py -3 probe_toolcycle.py [COM11] [n_cycles]
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
NCYC = int(sys.argv[2]) if len(sys.argv) > 2 else 20


def heapvals(h):
    d = h.heap()
    # tolerate field-name variance across builds
    dram = d.get("dram", d.get("free", d.get("free_heap", 0)))
    psram = d.get("psram", d.get("free_psram", d.get("spiram", 0)))
    return dram, psram, d


def timed(fn):
    t0 = time.perf_counter()
    r = fn()
    return (time.perf_counter() - t0), r


def baseline(h, label):
    dram, psram, raw = heapvals(h)
    st, _ = timed(h.state)
    print(f"  [{label}] dram={dram:,}  psram={psram:,}  state={st*1000:6.0f}ms  raw={raw}")
    return dram


def wait_boot(port, tries=30):
    """Open a session and poll ping until the badge answers (post-reset boot)."""
    for i in range(tries):
        try:
            h = Harness(port=port)
            r = h.ping()
            if r.get("ok"):
                print(f"[boot] up after {i+1} connect attempt(s): {r}")
                return h
            h.close()
        except Exception as e:
            print(f"[boot] attempt {i+1}: {e}")
        time.sleep(2)
    raise RuntimeError("badge never came up")


def main():
    print(f"=== probe_toolcycle  port={PORT}  cycles={NCYC} ===")
    h = wait_boot(PORT)
    try:
        h.nav(1, 0); h.wait(500)          # ITEMS > Tools
        scan = h.cat_pos("Scan")
        if scan is None:
            print("ERROR: no 'Scan' category (SKU?) -- cats:",
                  [c['name'] for c in h.cmd('tool_list').get('cats', [])])
            return
        print(f"[info] Scan category at pos {scan}")

        base = baseline(h, "fresh baseline")

        print(f"\n  cycle |     dram   d(base) |   psram | tool_start | state")
        print(  f"  ------+-------------------+---------+------------+-------")
        for i in range(NCYC):
            h.nav(1, 0)
            ts, r = timed(lambda: h.tool_start(scan, 0))
            h.wait(900)
            h.tool_stop()
            h.wait(400)
            dram, psram, _ = heapvals(h)
            sst, _ = timed(h.state)
            flag = "" if r.get("ok") else "  <-- tool_start FAIL"
            print(f"  {i+1:>4}  | {dram:>9,} {dram-base:>+8,} | {psram:>7,} | "
                  f"{ts*1000:>8.0f}ms | {sst*1000:>4.0f}ms{flag}")

        print("\n[reboot] soft reboot to test reversibility...")
        h.tool_stop()
        h.reboot_and_wait(timeout=22)
        baseline(h, "post-reboot baseline")
    finally:
        h.close()


if __name__ == "__main__":
    main()
