#!/usr/bin/env python3
"""verify_fixes.py -- runtime-verify the three overnight-finding fixes on COM11.
  1) Evil Portal DRAM leak: start/stop cycles should stop re-leaking.
  2) Active-TX UI FPS: Beacon Funny/Random, Flood Auth, Deauth should be smooth
     and start without the 7-10s stall.
  3) CDC session wedge: a tight tool_open+screenshot+cmd loop should not desync.
"""
import sys, os, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "12")
from harness import Harness

def dram(h):
    r = h.heap(); return r.get("dram")

def test_evil_portal(h):
    print("\n=== 1) Evil Portal heap (start/stop x6) ===")
    base = dram(h); prev = base
    print(f"  baseline DRAM={base}")
    worst_cycle = 0
    for i in range(6):
        h.cmd("tool_open 11 0"); time.sleep(2.2)
        h.tool_stop(); time.sleep(1.3)
        d = dram(h); net = d - prev
        print(f"  cycle {i+1}: after-stop DRAM={d} (net {net:+d})")
        if i >= 1: worst_cycle = min(worst_cycle, net)  # ignore cycle1 (one-time AsyncTCP infra)
        prev = d
    print(f"  -> worst per-cycle leak AFTER cycle 1: {worst_cycle} B "
          f"(PASS if >= -600; one-time infra shows only on cycle 1)")

def test_fps(h):
    print("\n=== 2) Active-TX UI FPS ===")
    for cid, item, name in [(8,4,"Beacon Funny"),(8,0,"Beacon Random"),(7,0,"Flood Auth"),(6,0,"Deauth")]:
        t0 = time.time()
        r = h.cmd(f"tool_open {cid} {item}")
        start_lat = time.time() - t0
        h.fps_reset()
        time.sleep(0.5)
        samples = []
        for _ in range(6):
            f = h.fps()
            if f.get("ok"): samples.append(f.get("fps", 0))
            time.sleep(0.6)
        h.tool_stop(); time.sleep(0.4)
        if samples:
            print(f"  {name:14s}: start={start_lat:4.1f}s  fps min/avg/max="
                  f"{min(samples)}/{round(sum(samples)/len(samples),1)}/{max(samples)}  running={r.get('running')}")
        else:
            print(f"  {name:14s}: start={start_lat:4.1f}s  NO FPS SAMPLES running={r.get('running')}")

def test_wedge(h, label, quiet):
    print(f"\n=== 3) CDC wedge stress ({label}) ===")
    h.cmd(f"quiet {1 if quiet else 0}")
    os.makedirs(os.path.join(HERE, "overnight_results", "_probe"), exist_ok=True)
    shot = os.path.join(HERE, "overnight_results", "_probe", "wedge.bmp")
    desyncs = 0; n = 30
    for i in range(n):
        h.cmd("tool_open 1 0")            # Scan APs (verbose during scan)
        try:
            s = h.screenshot(shot)         # binary transfer -- the collision trigger
            if not s.get("ok"): desyncs += 1
        except Exception:
            desyncs += 1
        for probe in ("heap","detect_counts","sd_exists /raw_0.pcap"):
            r = h.cmd(probe)
            # a desynced response lacks the expected key
            key = {"heap":"dram","detect_counts":"ap","sd_exists /raw_0.pcap":"exists"}[probe]
            if key not in r: desyncs += 1
        h.tool_stop()
    print(f"  {n} iters (tool_open+screenshot+3 probes each) -> {desyncs} desynced responses "
          f"(PASS if 0)")

def main():
    h = Harness(port="COM11", skip_boot=True)
    try:
        print("SKU cats:", len(h.cmd("tool_list").get("cats", [])))
        test_evil_portal(h)
        test_fps(h)
        test_wedge(h, "quiet OFF (tests firmware auto-quiet around screenshot)", quiet=False)
        test_wedge(h, "quiet ON (session-wide)", quiet=True)
        h.cmd("quiet 0")
    finally:
        h.close()

if __name__ == "__main__":
    main()
