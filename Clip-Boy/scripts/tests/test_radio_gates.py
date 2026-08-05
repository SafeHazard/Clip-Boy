#!/usr/bin/env py -3
"""Radio station unlock-gate test (DC34-129 SegFault-Tec FM).

The 5 stations gate purely on collectible COUNT (radio.h gate_count spread:
RD0-SH0K=75, WLAN-FM=1, Tab Street=25, WGHOUL=50, pi-r8=0). This drives the
count via COLL_DEBUG (`coll reset` / `coll add <id>` / `coll add all`, PLAIN
text) and reads the live `gated_unlocked` mask from the `radio_announce_dump`
harness cmd (STX-framed), then asserts the mask matches the expected gate
spread at each threshold — proving the gating is a real per-station spread,
not all-or-nothing.

  py -3 scripts/tests/test_radio_gates.py [--port COM11]

Needs a --test build (TEST_HARNESS + COLL_DEBUG). Single dtr=False session.
Leaves collectibles RESET on exit.
"""
import argparse, sys, time, re
import serial
import serial.tools.list_ports as lp

STX = b"\x02"
# station index -> gate_count (from radio.h radio_stations[], dial order)
GATE = {0: 75, 1: 1, 2: 25, 3: 50, 4: 0}   # 0=RD0-SH0K 1=WLAN-FM 2=TabStreet 3=WGHOUL 4=pi-r8
NAMES = {0: "RD0-SH0K", 1: "WLAN-FM", 2: "Tab Street", 3: "WGHOUL", 4: "pi-r8 r4di0"}

def expected_gated_mask(count):
    """bits for gate_count>0 stations unlocked at `count` — matches
    th_cmd_radio_announce_dump (bit i, only when gate_count>0)."""
    m = 0
    for i, g in GATE.items():
        if g > 0 and count >= g:
            m |= (1 << i)
    return m

def find_port(p):
    if p: return p
    for d in lp.comports():
        if (d.vid in (0x303A, 0x1A86)) or "USB" in (d.description or ""): return d.device
    return None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port"); a = ap.parse_args()
    port = find_port(a.port)
    if not port: print("no badge found"); return 2
    s = serial.Serial(); s.port = port; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0.2; s.write_timeout = 4
    s.open(); time.sleep(0.6); s.reset_input_buffer()

    def read_until(sentinel, idle=0.35, cap=10.0):
        """Read until `sentinel` appears AND the stream goes quiet (lockstep — no
        buffer reset, so send/read never shift under load)."""
        buf = bytearray(); t0 = time.time(); last = time.time()
        while time.time() - t0 < cap:
            chunk = s.read(4096)
            if chunk:
                buf += chunk; last = time.time()
            else:
                if sentinel in buf and time.time() - last >= idle:
                    break
                if time.time() - last >= cap * 0.5:   # sentinel never came -> give up
                    break
        return buf.decode("utf-8", "replace")

    def plain(c, idle=0.25, cap=12.0):
        s.write((c + "\n").encode()); s.flush()
        return read_until(b"> ", idle, cap)

    def th(c, idle=0.25, cap=4.0):
        s.write(STX + c.encode() + b"\n"); s.flush()
        return read_until(STX, idle, cap)

    fails = []
    def check(name, ok, extra=""):
        print(("  PASS " if ok else "  FAIL ") + name + (" " + extra if extra else ""))
        if not ok: fails.append(name)

    def coll_count():
        for _ in range(3):
            out = plain("coll list", idle=0.4, cap=14.0)
            rows = len(re.findall(r"\[[\* ]\]", out))
            n = len(re.findall(r"\[\*\]", out))
            if rows == 95:            # exactly one full list -> trustworthy
                return n
            time.sleep(0.4)
        return n

    def gated_mask():
        for _ in range(3):
            out = th("radio_announce_dump")
            m = re.search(r'"gated_unlocked":(\d+)', out)
            syn = re.search(r'"synced":(true|false)', out)
            if m:
                return (int(m.group(1)), syn.group(1) if syn else "?")
            time.sleep(0.3)
        return (-1, "?")

    def unlocked_names(count):
        return [NAMES[i] for i in range(5) if count >= GATE[i]]

    print(f"[radio-gates] port {port}")
    ids = [int(x) for x in re.findall(r"^\s+(\d+)\s+.*\[[\* ]\]",
                                      plain("coll list", idle=0.5, cap=10.0), re.M)]
    print(f"  catalog: {len(ids)} collectibles")

    # threshold 0: nothing found -> only pi-r8 (gate 0) live
    plain("coll reset"); c = coll_count(); g, syn = gated_mask()
    check("count=0 baseline", c == 0, f"(count {c})")
    check("count=0 -> only pi-r8 unlocked (gated mask 0)", g == expected_gated_mask(0) == 0,
          f"(mask {g}, unlocked {unlocked_names(0)})")

    # threshold 1: WLAN-FM unlocks, higher tiers still locked (proves spread)
    if ids: plain(f"coll add {ids[0]}")
    c = coll_count(); g, _ = gated_mask()
    check("count=1 -> WLAN-FM only (mask 2)", g == expected_gated_mask(c),
          f"(count {c}, mask {g} exp {expected_gated_mask(c)}, unlocked {unlocked_names(c)})")

    # mid: add ~30 more real ids to cross gate 25 (Tab Street) but stay under 50/75.
    # Proves a HIGHER threshold gates partially: Tab Street IN, WGHOUL+RD0-SH0K OUT.
    for i in ids[1:31]:
        plain(f"coll add {i}", idle=0.2, cap=3.0)
    time.sleep(0.5)
    c = coll_count(); g, _ = gated_mask()
    check("mid ~30: Tab Street (gate 25) unlocked (bit2)", (g & 4) != 0, f"(count {c}, mask {g})")
    check("mid ~30: WGHOUL (gate 50) still LOCKED (bit3 clear)", (g & 8) == 0, f"(mask {g})")
    check("mid ~30: RD0-SH0K (gate 75) still LOCKED (bit0 clear)", (g & 1) == 0, f"(mask {g})")
    check("mid ~30: PARTIAL spread mask==6", g == 6, f"(mask {g}, expect 6)")

    # full: coll add all -> count>=75 -> all 5 unlocked + announce synced
    plain("coll add all", idle=0.5, cap=12.0); c = coll_count(); g, syn = gated_mask()
    check("coll add all -> count>=75", c >= 75, f"(count {c})")
    check("all 4 gated unlocked (mask 15)", g == 15, f"(mask {g})")
    check("all 5 stations unlocked", len(unlocked_names(c)) == 5, f"({unlocked_names(c)})")
    check("announce mask synced after bulk add", syn == "true", f"(synced {syn})")

    # cleanup
    plain("coll reset"); rc = coll_count()
    check("cleanup: collectibles reset", rc == 0, f"(count {rc})")

    s.close()
    print("\n[radio-gates] " + ("ALL PASS — station gating works" if not fails
                                else f"FAILED: {fails}"))
    return 0 if not fails else 1

if __name__ == "__main__":
    sys.exit(main())
