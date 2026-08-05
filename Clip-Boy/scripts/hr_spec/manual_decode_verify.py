#!/usr/bin/env python3
"""Round-trip every catalog collectible id through the badge's manual-entry
decoder (feature/manual-entry-grid, Task 1).

For each id in data/collectibles.csv this:
  1. Builds the DECODER-space 4x4 anchor grid (grid_for() = SECDED encoder +
     anchors/guard, the same producer used for per-id hardness scoring).
  2. X-mirrors it to the USER-view the operator sees (TL/TR/BR raised, BL flat):
        user[r][c] = decoder[r][3-c]
  3. Flattens row-major to 16 '0'/'1' chars and sends `manual_decode <chars>`,
     asserting the badge returns the SAME id (clean decode).
  4. Flips ONE data cell -> asserts it STILL returns the same id (1-bit SECDED
     correction).
  5. Flips TWO data cells -> asserts `invalid` (2-bit SECDED rejection).

Requires a --test build (TEST_HARNESS) on the badge. Port from CB_PORT env, else
auto-detected (falls back to COM10). Opens with dtr=False so it doesn't reset the
native-USB-CDC badge (see id_sweep.py).

  CB_PORT=COM10 py -3 manual_decode_verify.py

This does NOT need a badge to be *correct* -- run it against real hardware to
validate the firmware round-trip.
"""
import os
import re
import sys
import csv
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pattern_hardness import grid_for, CELL_ORDER   # grid_for = SECDED anchor grid

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("pyserial required: py -3 -m pip install pyserial")
    sys.exit(2)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CSV_PATH = os.path.join(REPO, "data", "collectibles.csv")
RESP_RE = re.compile(r"manual_decode:\s*(?:id=(\d+)|(invalid))")


def find_port():
    p = os.environ.get("CB_PORT")
    if p:
        return p
    for info in list_ports.comports():
        desc = (info.description or "") + (info.manufacturer or "")
        if any(k in desc for k in ("USB", "CDC", "Serial", "ESP", "Espressif")):
            return info.device
    return "COM10"


def catalog_ids():
    ids = []
    with open(CSV_PATH, newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        next(r, None)  # header (ID,Title,...)
        for row in r:
            if not row:
                continue
            tok = row[0].strip()
            if tok.isdigit():
                ids.append(int(tok))
    return ids


def user_chars(decoder_grid):
    """X-mirror decoder-space 4x4 -> user-view, flatten row-major to 16 chars."""
    return "".join(
        "1" if decoder_grid[r][3 - c] else "0"
        for r in range(4) for c in range(4)
    )


class Badge:
    def __init__(self, port):
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.dtr = False          # native-USB-CDC: no reset-on-open
        s.rts = False
        s.timeout = 0.3
        s.open()
        time.sleep(0.3)
        s.reset_input_buffer()
        self.s = s

    def manual_decode(self, chars):
        """Send one manual_decode; return int id, or None for 'invalid'."""
        self.s.reset_input_buffer()
        self.s.write(b"\x02manual_decode " + chars.encode() + b"\n")
        self.s.flush()
        t0 = time.time()
        while time.time() - t0 < 1.0:
            line = self.s.readline().decode("ascii", "replace")
            if not line:
                continue
            m = RESP_RE.search(line)
            if m:
                return None if m.group(2) else int(m.group(1))
        raise TimeoutError("no manual_decode response")

    def close(self):
        self.s.close()


def main():
    ids = catalog_ids()
    if not ids:
        print("no catalog ids parsed from", CSV_PATH)
        return 2
    port = find_port()
    print(f"port={port}  ids={len(ids)} ({ids[0]}..{ids[-1]})")
    badge = Badge(port)

    fails = 0
    for idv in ids:
        dg = grid_for(idv)

        # (1) clean round-trip
        got = badge.manual_decode(user_chars(dg))
        if got != idv:
            print(f"FAIL id={idv}: clean decode returned {got}")
            fails += 1
            continue

        # (2) single data-cell flip -> still corrects to the same id
        r1, c1 = CELL_ORDER[0]
        g1 = [row[:] for row in dg]
        g1[r1][c1] ^= 1
        got1 = badge.manual_decode(user_chars(g1))
        if got1 != idv:
            print(f"FAIL id={idv}: 1-bit flip returned {got1} (expected corrected {idv})")
            fails += 1
            continue

        # (3) double data-cell flip -> rejected as invalid
        r2, c2 = CELL_ORDER[1]
        g2 = [row[:] for row in dg]
        g2[r1][c1] ^= 1
        g2[r2][c2] ^= 1
        got2 = badge.manual_decode(user_chars(g2))
        if got2 is not None:
            print(f"FAIL id={idv}: 2-bit flip returned id={got2} (expected invalid)")
            fails += 1
            continue

    badge.close()
    n = len(ids)
    print(f"\n{n - fails}/{n} ids passed (clean + 1-bit-correct + 2-bit-reject)")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
