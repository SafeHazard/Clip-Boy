#!/usr/bin/env python3
"""verify_extras.py -- verify the two optional follow-ups on COM11.
  Item 1: Flock(0.3)/BLE-Adverts(1.4)/Channel-Stats(2.3) now verifiable via new readback.
  Item 2: Evil Portal 16KB AsyncTCP reclaim -- heap fully recovers per cycle + stable
          (incl. client traffic at teardown, the worst-case for the task kill).
"""
import sys, os, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "20")
from harness import Harness
import tool_suite as TS
import overnight_integration as OI

def test_readback(dut):
    print("\n=== Item 1: 3 tools via new readback ===")
    PL = {(t["cid"], t["item"]): t for t in OI.PLAYLIST}
    for ci in [(0, 3), (1, 4), (2, 3)]:
        tool = PL[ci]
        fire, verify, note = OI._stim_for(tool)
        dut.cmd(f"tool_open {ci[0]} {ci[1]}")
        time.sleep(0.6)
        base = OI._read_src(dut, verify[0])
        try:
            fire()
        except Exception as e:
            print(f"  {ci[0]}.{ci[1]} stim error: {e}")
        time.sleep(1.0)
        final = OI._read_src(dut, verify[0])
        obs, ok = OI._verify_delta(base, final, verify[1], verify[3], verify[2])
        dut.tool_stop(); time.sleep(0.5)
        print(f"  {ci[0]}.{ci[1]} {tool['name']:14s}: {verify[0]}.{verify[1]} "
              f"base={base.get(verify[1]) if isinstance(verify[1],str) else '?'} obs={obs} "
              f"thr={verify[3]} -> {'PASS' if ok else 'FAIL'} ({note})")

def test_evilportal_reclaim(dut):
    print("\n=== Item 2: Evil Portal 16KB reclaim soak (10 cycles) ===")
    def dram(): return dut.heap().get("dram")
    def uptime():
        p = dut.ping(); return p.get("uptime") if p.get("ok") else None
    base = dram(); u0 = uptime()
    print(f"  baseline DRAM={base} uptime={u0}")
    prev = base
    reboot_any = False; worst_after1 = 0
    for i in range(10):
        client = (i % 3 == 0)   # every 3rd cycle: generate AP-side lwIP traffic at teardown
        dut.cmd("tool_open 11 0"); time.sleep(2.0)
        if client:
            try: TS.kalipi("wifi-scan", timeout=35)   # probes/assoc attempts toward the portal AP
            except Exception: pass
        dut.tool_stop(); time.sleep(1.5)
        d = dram(); u = uptime()
        reboot = (u is not None and u0 is not None and u < u0)
        alive = (u is not None)
        net = d - prev
        if i >= 1: worst_after1 = min(worst_after1, net)   # cycle 1 carries the one-time -16KB task
        if reboot: reboot_any = True
        print(f"  cycle {i+1:2d}{' [client]' if client else '        '}: DRAM={d} (net {net:+d}) "
              f"alive={alive} reboot={reboot}")
        prev = d
        if reboot or not alive:
            print("  !! INSTABILITY -> aborting soak"); break
    # 16KB one-time AsyncTCP task retention (cycle 1) is EXPECTED/by-design now.
    # Success = NO reboot across all cycles + per-cycle leak after cycle 1 is ~0.
    verdict = "STABLE, no per-activation leak" if (not reboot_any and worst_after1 >= -600) \
              else ("REBOOTED (unstable)" if reboot_any else f"per-cycle leak {worst_after1}B")
    print(f"  -> {verdict} (one-time task retention ~16KB on cycle 1 is by-design)")
    return not reboot_any and worst_after1 >= -600

def main():
    dut = Harness(port="COM11", skip_boot=True)
    try:
        print("SKU cats:", len(dut.cmd("tool_list").get("cats", [])))
        test_readback(dut)
        test_evilportal_reclaim(dut)
    finally:
        dut.close()
        try: TS.kalipi_restore()
        except Exception: pass

if __name__ == "__main__":
    main()
