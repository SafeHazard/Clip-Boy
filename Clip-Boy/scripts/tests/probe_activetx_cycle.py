#!/usr/bin/env python3
"""probe_activetx_cycle.py -- characterize whether the badge DEGRADES over active-TX
tool start/stop cycles (the tool_gauntlet flakiness). Reboots clean, then cycles a mix
of active-TX tools with recovery, logging per-cycle: free DRAM, tool_start latency, a
trivial state latency, and the tool_list category count (which came back inconsistent
in the gauntlet). Monotonic DRAM decline / growing latency / shrinking cat count =>
real accumulation. Flat => inherent per-tool cost."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

PORT = os.environ.get("CLIPBOY_PORT", "COM11")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 18

h = Harness(port=PORT)
h.reboot_and_wait(timeout=24)
if os.environ.get("CB_QUIET"):
    print("quiet:", h.cmd("quiet 1"))
print(f"booted uptime={h.ping().get('uptime')}")

# active-TX tools to rotate (cat name, item)
seq = [("Beacon Spam", 4), ("Deauth", 0), ("Flood", 1), ("BLE Spam", 0), ("Beacon Spam", 0)]
pos = {}
def cat(name):
    if name not in pos: pos[name] = h.cat_pos(name)
    return pos[name]

def catcount():
    return len(h.cmd("tool_list").get("cats", []))

base = h.heap().get("dram", 0)
print(f"baseline dram={base:,} cats={catcount()}")
print("cycle | dram      d(base) | tool_start | state | cats")
print("------+-------------------+------------+-------+-----")
for i in range(N):
    name, item = seq[i % len(seq)]
    c = cat(name)
    if c is None:
        print(f" {i+1:>4} | cat '{name}' MISSING (badge degraded?)"); continue
    h.nav(1, 0); h.wait(200)
    t0 = time.perf_counter(); r = h.tool_start(c, item); ts = time.perf_counter() - t0
    h.wait(1200)
    st0 = time.perf_counter(); s = h.state(); sst = time.perf_counter() - st0
    h.tool_stop(); h.wait(800)                       # recovery (like the gauntlet fix)
    dram = h.heap().get("dram", 0); cc = catcount()
    flag = "" if r.get("ok") else "  <-- tool_start FAIL"
    print(f" {i+1:>4} | {dram:>9,} {dram-base:>+8,} | {ts*1000:>8.0f}ms | {sst*1000:>4.0f}ms | {cc:>3}{flag}")
h.tool_stop(); h.close()
