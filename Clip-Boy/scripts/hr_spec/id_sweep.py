#!/usr/bin/env python3
"""Empirical per-tag reliability check — the DEFINITIVE reliability test.

The reliability simulator (reliability_sim.py) proved that a printed tag's scan
reliability is dominated by PHYSICAL factors (print registration, per-zone sensor
response) that no bit-pattern model captures: id 37 and id 101 are geometrically
near-identical yet 37 scans and 101 fails. So the only trustworthy way to know if
an ID is usable is to scan its ACTUAL printed tag. This tool does that fast.

Present the printed tag to the badge (~70mm, filling the view), then run:

    py -3 id_sweep.py <expected_id> [seconds]        # default 12s
    CB_PORT=COM10 py -3 id_sweep.py 64

It scans, watches the decode stream, and reports the fraction of frames that
decode to the tag's OWN id with SECDED syndrome 0 (the firmware's lock condition),
plus what else it saw. Appends one row to id_sweep_results.csv so a morning sweep
builds a reliable-ID list directly consumable by ../remap_collectibles.py
(reliable_ids.txt = the PASS ids, best-rate first).

Verdicts (clean-self-decode rate over the window):
  >= 0.30  GOOD    (locks fast, like 47/55/64/88)
  0.10-0.30 MARGINAL (locks but slow, like 16)
  < 0.10   FAIL    (never locks, or mis-decodes -- like 43/101)
The never-wrong guard means a FAIL tag is SAFE (won't wrong-unlock); it just
won't scan, so reprint or reassign it.
"""
import os
import re
import sys
import time

import serial

PORT = os.environ.get("CB_PORT", "COM10")
DECODE_RE = re.compile(r"decode: id=(\d+) status=(\w+) syn=(\d+)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    expected = int(sys.argv[1])
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0

    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.dtr = False           # native-USB-CDC: no reset-on-open
    s.rts = False
    s.timeout = 0.3
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()

    def send(c, w=0.3):
        s.write(b"\x02" + c.encode() + b"\n")
        s.flush()
        time.sleep(w)

    send("hr_anchor 1")
    send("nav 1 1", 0.4)          # ITEMS > Collectibles
    send("hr_scan_start", 0.6)

    clean = total = 0
    seen = {}
    t0 = time.time()
    while time.time() - t0 < secs:
        # POLL hr_debug -- the badge emits decode frames ONLY on request, not
        # passively. (The old passive readline() saw nothing -> every ID scored
        # FAIL/0-frames regardless of the tag. This is the fix.)
        s.write(b"\x02hr_debug\n"); s.flush()
        resp = ""; t1 = time.time()
        while time.time() - t1 < 0.3:
            n = s.in_waiting
            if n:
                resp += s.read(n).decode(errors="replace")
            else:
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
    s.close()

    rate = clean / total if total else 0.0
    verdict = "GOOD" if rate >= 0.30 else "MARGINAL" if rate >= 0.10 else "FAIL"
    top = sorted(seen.items(), key=lambda x: -x[1])[:3]
    saw = ", ".join(f"{i}x{n}" for i, n in top) or "nothing"       # stdout (commas, readable)
    saw_csv = ";".join(f"{i}x{n}" for i, n in top) or "nothing"    # CSV (semicolons, no quoting)
    print(f"id {expected}: {verdict}  clean-self rate={rate:.2f}  "
          f"({total} decoded frames; saw {saw})")

    logp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "id_sweep_results.csv")
    new = not os.path.exists(logp)
    with open(logp, "a") as f:
        if new:
            f.write("id,verdict,clean_self_rate,frames,saw\n")
        # saw = the top-3 decoded IDs (id x count) -- so a reader can see WHAT a tag
        # aliased to (wrong-lock risk) without the console. Semicolon-separated.
        f.write(f"{expected},{verdict},{rate:.2f},{total},{saw_csv}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
