#!/usr/bin/env python3
"""Guided HR-tag sweep harness — sets up + scores every tag with timing.

For each ID in the sweep: prints the OmniTag slider pattern + the expected item
name, waits for you to dial it in and press Enter, then drives the badge scan and
records the outcome (LOCK / WRONG-LOCK / TIMEOUT), the decoded ID, time-to-lock
(measured from the FIRST processed frame, so LiDAR spin-up isn't counted), the
lock distance, and clean-frame count. Appends a row to sweep_log.csv.

Usage (badge on COM10, --test build, OmniTag ready):
  py -3 sweep_harness.py all              # every catalog ID
  py -3 sweep_harness.py 15 99 43         # just these
  py -3 sweep_harness.py 40-60            # a range
  CB_PORT=COM10 TIMEOUT=30 py -3 sweep_harness.py all

Notes
- Pattern shown is the PHYSICAL slider view (already un-mirrored: bump-side to
  sensor), so set the sliders exactly as drawn -- '#'=raise(near/1), '.'=down(far/0).
  Corners are fixed structure (A=anchor, g=guard) -- don't touch them.
- Enter to scan, 's'+Enter to skip, 'q'+Enter to quit early (log is saved as you go).
"""
import csv
import os
import re
import sys
import time

import serial
_HERE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE_DIR)                                  # for pattern_hardness
sys.path.insert(0, os.path.join(_HERE_DIR, ".."))             # for test_bridge (auto-port)
from pattern_hardness import grid_for


def _resolve_port():
    """CB_PORT env wins; else auto-detect the ESP32-S3 by USB VID; else COM10."""
    p = os.environ.get("CB_PORT")
    if p:
        return p
    try:
        from test_bridge import find_esp32_port
        return find_esp32_port() or "COM10"
    except Exception:
        return "COM10"


PORT = _resolve_port()
TIMEOUT = float(os.environ.get("TIMEOUT", "30"))
LOCKPOLICY = os.environ.get("CB_LOCKPOLICY", "1")   # 1 = vote-lock (default), 0 = legacy gate (A/B)
HERE = os.path.dirname(os.path.abspath(__file__))
CSVP = os.path.join(HERE, "..", "..", "data", "collectibles.csv")
LOGP = os.path.join(HERE, "sweep_log.csv")
DEC_RE = re.compile(r"decode: id=(\d+) status=(\w+) syn=(\d+)")
LOCK_RE = re.compile(r"lockedId=(-?\d+)")
NEAR_RE = re.compile(r"near=(\d+)mm")
HEAP_RE = re.compile(r'"dram":(\d+).*?"min_dram":(\d+)')
# The loop's definitive verdict when the scan auto-stops (lock/timeout/unknown):
#   "[HR] Scan ended: timedOut=0 lockedId=15 unknownId=-1 -> SUCCESS"
END_RE = re.compile(r"Scan ended:.*?lockedId=(-?\d+) unknownId=(-?\d+) -> (\w+)")


def load_catalog():
    m = {}
    with open(CSVP, newline="", encoding="utf-8") as f:
        r = csv.reader(f); h = next(r)
        ic, tc = h.index("ID"), h.index("Title")
        for row in r:
            m[int(row[ic])] = row[tc]
    return m


def physical_pattern(idv):
    """grid_for(id) as-is = hm_codegen's layout. You set the OmniTag bump-side AWAY
    from you (toward the sensor), and that push applies the mirror physically -- so
    set the sliders exactly as drawn here. Guard 'g' is bottom-RIGHT."""
    g = grid_for(idv)
    corners = {(0, 0): "A", (0, 3): "A", (3, 0): "A", (3, 3): "g"}
    out = []
    for r in range(4):
        row = []
        for c in range(4):
            if (r, c) in corners:
                row.append(corners[(r, c)])
            else:
                row.append("#" if g[r][c] else ".")
        out.append(" ".join(row))
    return out


def parse_ids(args, catalog):
    if args == ["all"]:
        return sorted(catalog)
    ids = []
    for a in args:
        if "-" in a and a.replace("-", "").isdigit():
            lo, hi = map(int, a.split("-")); ids += list(range(lo, hi + 1))
        else:
            ids.append(int(a))
    # Keep any valid HR id (0-127), NOT just catalog ids -- the full reliability
    # sweep must cover the free/unused ids too (they're the reassignment candidates).
    return [i for i in ids if 0 <= i <= 127]


def _send(s, c, w=0.3):
    """Send an STX-framed cmd; return everything read back over w seconds."""
    s.write(b"\x02" + c.encode() + b"\n"); s.flush()
    buf = ""; t = time.time()
    while time.time() - t < w:
        chunk = s.read(256)
        buf += chunk.decode(errors="replace") if chunk else ""
        if not chunk:
            time.sleep(0.01)
    return buf


def _drain(s, dur):
    """Wait `dur` seconds WHILE reading+discarding -- never let the --test build's
    verbose logging fill the badge's USB-CDC TX buffer (a blind sleep stalls the badge)."""
    t = time.time()
    while time.time() - t < dur:
        try:
            n = s.in_waiting
            if n:
                s.read(n)
            else:
                time.sleep(0.02)
        except Exception:
            time.sleep(0.02)


def _heap(s):
    """Free internal heap (dram) + all-time LOW-WATER (min_dram), bytes. min_dram is
    monotonic-down -- a steady scan-by-scan decline = a leak (e.g. stacked modals)."""
    m = HEAP_RE.search(_send(s, "heap", 0.4))
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def scan_one(s, expected):
    # NO auto-dismiss: dismissing the success modal from the serial context
    # (hr_dismiss -> hr_scan_finish_modal -> rebuild_content) rebooted the badge.
    # Instead YOU tap the badge to dismiss it (the safe UI path) before pressing
    # Enter, so no modal is up when we start -- and none can stack.
    _send(s, "hr_scan_stop", 0.3)                         # clear any prior scan (no-op if idle)
    _send(s, "hr_anchor 1", 0.2)
    # A/B: pin the lock policy each scan (reflash/reboot resets it to the default
    # vote-lock). CB_LOCKPOLICY=1 = vote-lock (new, default), 0 = legacy gate.
    _send(s, f"hr_lockpolicy {LOCKPOLICY}", 0.2)
    _send(s, "nav 1 1", 0.4)                              # ensure on Collectibles
    _send(s, "hr_scan_start", 0.6)                        # start ONCE -- poll loop confirms via frames
    t0 = time.time(); first = None; end_t = None
    locked = -1; unknown = -1; path = "TIMEOUT"; near = 0; clean = 0; frames = 0; seen = {}
    # Wait a bit PAST the badge's own ~30s scan timeout so its "Scan ended" line
    # (SUCCESS / TIMEOUT / UNKNOWN) always fires while we're still reading.
    while time.time() - t0 < TIMEOUT + 6:
        d = _send(s, "hr_debug", 0.3)                     # POLL -- badge dumps on request
        for m in DEC_RE.finditer(d):
            if first is None:
                first = time.time()                       # first real frame = spin-up done
            did, st, syn = int(m.group(1)), m.group(2), int(m.group(3))
            if st == "Ok":
                frames += 1; seen[did] = seen.get(did, 0) + 1
                if did == expected and syn == 0:
                    clean += 1
        nm = list(NEAR_RE.finditer(d))
        if nm:
            near = int(nm[-1].group(1))
        em = END_RE.search(d)                             # definitive verdict from the loop
        if em:
            locked, unknown, path = int(em.group(1)), int(em.group(2)), em.group(3)
            end_t = time.time(); break
        lm = LOCK_RE.search(d)                            # fallback: hr_debug lockedId transition
        if lm and int(lm.group(1)) >= 0:
            locked = int(lm.group(1)); path = "SUCCESS"; end_t = time.time(); break
    _send(s, "hr_scan_stop", 0.2)
    ttl = (end_t - first) if (first is not None and locked >= 0 and end_t) else None
    top = sorted(seen.items(), key=lambda x: -x[1])[:3]
    return dict(locked=locked, unknown=unknown, path=path, ttl=ttl, near=near,
                clean=clean, frames=frames, saw=";".join(f"{i}x{n}" for i, n in top))


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    cat = load_catalog()
    ids = parse_ids(sys.argv[1:], cat)
    s = serial.Serial(); s.port = PORT; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0.25; s.open(); time.sleep(0.3)
    print(f"[badge on {PORT}]  lock policy = {'VOTE-LOCK' if LOCKPOLICY != '0' else 'LEGACY'} (CB_LOCKPOLICY={LOCKPOLICY})")
    _send(s, "cfg_set disp_off 5", 0.3)               # screensaver -> Never (don't sleep mid-sweep)
    _send(s, "touch 160 120 press", 0.1); time.sleep(1.2); _send(s, "touch 160 120 release", 0.4)  # wake
    new = not os.path.exists(LOGP)
    log = open(LOGP, "a", newline="")
    w = csv.writer(log)
    if new:
        w.writerow(["id", "name", "outcome", "locked_id", "locked_name",
                    "time_to_lock_s", "lock_dist_mm", "clean_frames", "frames", "saw", "free_kb", "min_kb"])
    heaps = []
    try:
        for n, idv in enumerate(ids, 1):
            print(f"\n===== [{n}/{len(ids)}]  ID {idv}  =>  {cat.get(idv, '(unused/free ID)')} =====")
            for row in physical_pattern(idv):
                print("   " + row)
            cmd = input("   tap-dismiss any badge popup + set sliders, ENTER to scan (s=skip, q=quit): ").strip().lower()
            if cmd == "q":
                break
            if cmd == "s":
                w.writerow([idv, cat.get(idv, "(unused/free ID)"), "SKIP", "", "", "", "", "", "", ""]); log.flush()
                continue
            r = scan_one(s, idv)
            if r["locked"] == idv:
                outcome = "LOCK"
            elif r["locked"] >= 0:
                outcome = "WRONG"                          # locked a different collectible
            elif r["unknown"] >= 0:
                outcome = "WRONG"                          # decoded a non-collectible ID
            else:
                outcome = "TIMEOUT"
            got = r["locked"] if r["locked"] >= 0 else r["unknown"]
            gname = (cat.get(got, "(non-collectible)") if got >= 0 else "")
            ttl = f"{r['ttl']:.1f}" if r["ttl"] is not None else ""
            dram, mind = _heap(s); heaps.append(mind)
            leak = len(heaps) >= 3 and mind < heaps[0] - 4096
            print(f"   -> {outcome}  got={got} ({gname})  "
                  f"t={ttl}s  dist={r['near']}mm  clean={r['clean']}/{r['frames']}  "
                  f"heap={dram//1024}k min={mind//1024}k" + ("  <-- LEAK?" if leak else ""))
            w.writerow([idv, cat.get(idv, "(unused/free ID)"), outcome, got, gname, ttl,
                        r["near"], r["clean"], r["frames"], r["saw"],
                        dram // 1024, mind // 1024]); log.flush()
    except KeyboardInterrupt:
        print("\n[interrupted -- log saved]")
    finally:
        log.close(); s.close()
    if heaps:
        drop = (heaps[0] - min(heaps)) // 1024
        print(f"\nheap low-water: start {heaps[0]//1024}k -> end {heaps[-1]//1024}k "
              f"(min {min(heaps)//1024}k, dropped {drop}k over {len(heaps)} scans) -- "
              + ("POSSIBLE LEAK, investigate" if drop > 8 else "stable, no leak"))
    print(f"\nlog -> {os.path.relpath(LOGP)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
