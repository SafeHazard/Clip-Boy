#!/usr/bin/env python3
"""ble_verify.py — verify the BLE 'All' spam fix: cold + warm starts stay fast,
single-types fine, and the spam actually airs (external kalipi BLE scan)."""
import sys, os, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

PORT = os.environ.get("CLIPBOY_PORT", "COM11")
h = Harness(port=PORT)
h.reboot_and_wait(timeout=24)
ble = h.cat_pos("BLE Spam")
results = []

def run(item, label, runms=2200, settle=1800):
    h.tool_stop(); h.wait(settle); h.nav(1, 0); h.wait(300)
    a = time.perf_counter(); r = h.tool_start(ble, item); dt = time.perf_counter() - a
    h.wait(runms); ts = h.tool_state()
    ok = dt < 3 and r.get("ok") and ts.get("running")
    results.append((label, dt, ok))
    print(f"  {label:18}: start {dt:5.2f}s ok={r.get('ok')} running={ts.get('running')}  {'PASS' if ok else 'FAIL'}", flush=True)

print(f"=== BLE fix verify on {PORT} ===", flush=True)
run(5, "All #1 (cold)")
run(5, "All #2 (warm)")
run(5, "All #3 (warm)")
run(0, "SourApple")
run(5, "All #4 (warm)")
run(2, "Samsung single")
run(5, "All #5 (warm)")
run(1, "Swiftpair single")
run(5, "All #6 (warm)")

# external adv proof: leave All running, scan from kalipi
h.tool_stop(); h.wait(600); h.nav(1, 0); h.wait(200); h.tool_start(ble, 5); h.wait(1500)
try:
    out = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                          "data@192.168.1.146", "python3 /tmp/kalipi_stim.py ble-scan 8 2>/dev/null"],
                         capture_output=True, text=True, timeout=25)
    print(f"  kalipi ble-scan: {out.stdout.strip()[:180]}", flush=True)
except Exception as e:
    print(f"  kalipi ble-scan error: {e}", flush=True)
h.tool_stop(); h.close()

npass = sum(1 for _, _, ok in results if ok)
print(f"\n=== {npass}/{len(results)} BLE start checks PASS ===", flush=True)
sys.exit(0 if npass == len(results) else 1)
