#!/usr/bin/env python3
"""full_sweep.py -- DEFINITIVE physical reliability sweep of every HR tag ID (0-127).

Why: printed/OmniTag scan reliability is a PHYSICAL property no bit-model predicts
(id 37 scans, id 101 fails despite near-identical geometry). The only trustworthy map
is to physically scan every ID. This walks all 128, one OmniTag setting at a time, and
records for each: clean-self-decode rate (the firmware's lock condition), what it
aliased to, and a verdict. The result (full_sweep_results.csv) is the ground truth for
assigning the 95 collectibles to the 95 most-reliable, non-colliding IDs.

Interactive + RESUMABLE + CHUNKABLE. It prints each ID's OmniTag grid inline, waits for
you to set the tag and press Enter, scans ~12s, logs, and moves on. Skips IDs already in
the CSV, so you can do it in sittings (Ctrl-C any time; rerun to continue).

  py -3 scripts/hr_spec/full_sweep.py                # all 0-127, resume where you left off
  py -3 scripts/hr_spec/full_sweep.py 0 31           # just IDs 0..31 (chunk it)
  py -3 scripts/hr_spec/full_sweep.py --redo 46 31   # force re-scan specific IDs
  py -3 scripts/hr_spec/full_sweep.py --secs 8       # shorter scan window per ID
  CB_PORT=COM10 py -3 scripts/hr_spec/full_sweep.py

Verdicts (clean-self rate = fraction of decoded frames that hit THIS id with syndrome 0):
  GOOD       self >= 0.30 AND self is the dominant decode  -> locks fast & correct
  MARGINAL   self 0.10-0.30                                -> locks slow / flaky
  WRONGLOCK  dominant decode is a DIFFERENT id (>=50%)     -> can wrong-unlock that id
  FAIL       self < 0.10, no single dominant               -> won't lock (fails safe)
"""
import os
import re
import sys
import csv
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from secded import encode
from pattern_hardness import grid_for

PORT = os.environ.get("CB_PORT", "COM10")
DECODE_RE = re.compile(r"decode: id=(\d+) status=(\w+) syn=(\d+)")
LOGP = os.path.join(HERE, "full_sweep_results.csv")
ANCHORS = {(0, 0), (0, 3), (3, 0)}


def grid_str(idv):
    g = grid_for(idv)
    out = []
    for r in range(4):
        out.append("   " + " ".join(
            "A" if (r, c) in ANCHORS else "G" if (r, c) == (3, 3)
            else "#" if g[r][c] else "." for c in range(4)))
    return "\n".join(out)


def scan_one(s, expected, secs):
    def send(c, w=0.3):
        s.write(b"\x02" + c.encode() + b"\n"); s.flush(); time.sleep(w)
    s.reset_input_buffer()
    # NO hr_dismiss here: it reboots the badge (confirmed -- crashed the sweep at ID 11
    # even with the nav-off guard, so it's the dismiss itself, not just rebuild_content).
    # id_sweep never dismisses and ran stable all session, so we accept the small
    # per-lock modal leak (cached image + a few widgets) instead. hr_scan_start
    # overwrites hr_blackout each scan; the orphaned overlays are lightweight.
    send("hr_anchor 1")
    send("nav 1 1", 0.4)          # ITEMS > Collectibles
    send("hr_scan_start", 0.6)
    clean = total = 0
    seen = {}
    t0 = time.time()
    while time.time() - t0 < secs:
        s.write(b"\x02hr_debug\n"); s.flush()   # badge emits decode frames only on request
        resp = ""; t1 = time.time()
        while time.time() - t1 < 0.3:
            n = s.in_waiting
            resp += s.read(n).decode(errors="replace") if n else ""
            if not n:
                time.sleep(0.02)
        for m in DECODE_RE.finditer(resp):
            did, status, syn = int(m.group(1)), m.group(2), int(m.group(3))
            if status != "Ok":
                continue
            total += 1
            seen[did] = seen.get(did, 0) + 1
            if did == expected and syn == 0:
                clean += 1
    send("hr_scan_stop", 0.2)
    return (clean / total if total else 0.0), total, seen


def verdict_for(rate, seen, expected):
    if not seen:
        return "FAIL", -1
    top = sorted(seen.items(), key=lambda x: -x[1])
    dom, domn = top[0]
    tot = sum(seen.values())
    if rate >= 0.30 and dom == expected:
        return "GOOD", dom
    if rate >= 0.10:
        return "MARGINAL", dom
    if dom != expected and domn >= 0.5 * tot:
        return "WRONGLOCK", dom
    return "FAIL", dom


def load_done():
    done = set()
    if os.path.exists(LOGP):
        with open(LOGP) as f:
            r = csv.reader(f); next(r, None)
            for row in r:
                if row and row[0].isdigit():
                    done.add(int(row[0]))
    return done


def main():
    args = sys.argv[1:]
    redo = "--redo" in args
    if redo:
        args.remove("--redo")
    secs = 12.0
    if "--secs" in args:
        i = args.index("--secs"); secs = float(args[i + 1]); del args[i:i + 2]
    if len(args) == 2 and all(a.isdigit() for a in args):
        ids = list(range(int(args[0]), int(args[1]) + 1))
    elif args:
        ids = [int(a) for a in args]
    else:
        ids = list(range(0, 128))

    done = set() if redo else load_done()
    todo = [i for i in ids if i not in done]
    if not todo:
        print("Nothing to do -- all requested IDs already in the CSV (use --redo to force).")
        return 0
    print(f"FULL SWEEP: {len(todo)} IDs to scan ({len(done)} already logged). Window {secs:.0f}s/ID.")
    print("Set the OmniTag to each shown grid, press Enter. (s=skip this ID, q=quit & save)\n")

    s = serial.Serial(); s.port = PORT; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0.3
    s.open(); time.sleep(0.3)
    new = not os.path.exists(LOGP)
    log = open(LOGP, "a", newline="")
    w = csv.writer(log)
    if new:
        w.writerow(["id", "verdict", "self_rate", "frames", "dominant", "self_is_dom", "saw"]); log.flush()

    tally = {}
    n = 0
    try:
        for idv in todo:
            n += 1
            print(f"===== ID {idv}  ({n}/{len(todo)}) =====   data={''.join(map(str, encode(idv)))}")
            print(grid_str(idv))
            ans = input("  [Enter]=scan   s=skip   q=quit : ").strip().lower()
            if ans == "q":
                break
            if ans == "s":
                continue
            rate, total, seen = scan_one(s, idv, secs)
            top = sorted(seen.items(), key=lambda x: -x[1])[:3]
            v, dom = verdict_for(rate, seen, idv)
            saw = ";".join(f"{i}x{c}" for i, c in top) or "nothing"
            tally[v] = tally.get(v, 0) + 1
            flag = {"WRONGLOCK": "  <== WRONG-LOCK RISK", "FAIL": "  (won't lock)",
                    "MARGINAL": "  (flaky)"}.get(v, "")
            print(f"  -> {v}  self={rate:.2f}  frames={total}  dominant={dom}  saw {saw}{flag}")
            print(f"     running: " + "  ".join(f"{k}={tally[k]}" for k in sorted(tally)) + "\n")
            w.writerow([idv, v, f"{rate:.2f}", total, dom, int(dom == idv), saw]); log.flush()
    except KeyboardInterrupt:
        print("\n[interrupted -- progress saved, rerun to resume]")
    finally:
        try:
            s.write(b"\x02hr_scan_stop\n"); s.flush()
        except Exception:
            pass
        s.close(); log.close()

    print(f"\nlog -> {os.path.relpath(LOGP)}   (this session: " +
          ", ".join(f"{k}={v}" for k, v in sorted(tally.items())) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
