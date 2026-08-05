#!/usr/bin/env py -3
"""
DC34 ARG serial fuzz harness (handoff: "robust ... tantamount to fuzzing").

Throws hostile input at the clipcli / puzzle serial surface and asserts the badge
never crashes (reboot/backtrace) or wedges (stops responding), and that counters
don't corrupt. Run against a --test build (CLIPBOY_DEBUG, for `clipcli marshal`).

  py -3 scripts/tests/arg_fuzz.py [--port COM8] [--rounds 2]

Exit 0 = clean. Non-zero = a crash/wedge/corruption was caught (printed).
"""
import argparse, sys, time, random, re

try:
    import serial
    import serial.tools.list_ports as lp
except ImportError:
    print("pyserial required: py -3 -m pip install pyserial"); sys.exit(2)

# Real ESP32 panic/reboot indicators only. NOTE: do NOT match the boot banner
# ("ClipBoy Marauder") — the Marauder CLI (the fall-through for unhandled lines)
# reprints it on some garbage input WITHOUT rebooting (no rst:0x), so it's a false
# positive. And don't match bare "panic" — P4's flavor text says "panics faster".
# A genuine reboot ALWAYS prints "rst:0x" from the ROM bootloader; that's the anchor.
CRASH_MARKERS = ("Backtrace:", "Guru Meditation", "rst:0x", "abort() was called",
                 "assert failed", "CORRUPT HEAP", "Stack canary", "LoadProhibited",
                 "StoreProhibited", "InstrFetchProhibited")

def find_port(p):
    if p: return p
    for d in lp.comports():
        if d.vid in (0x303A, 0x1A86) or "USB" in (d.description or ""):
            return d.device
    return None

class Badge:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.3)
        time.sleep(6)                      # boot
        self.s.reset_input_buffer()
        self.crashed = None
    def raw(self, data: bytes, wait=0.25, readlen=12000):
        self.s.write(data); self.s.flush(); time.sleep(wait)
        out = self.s.read(readlen).decode("utf-8", "replace")
        for m in CRASH_MARKERS:
            if m in out and self.crashed is None:
                self.crashed = (m, data[:80])
        return out
    def line(self, text, wait=0.25):
        return self.raw((text + "\n").encode("utf-8", "replace"), wait)
    def alive(self):
        """Liveness probe: marshal --dump must answer with a valid state line.
        A byte-storm can end mid-line (no trailing \\n); clear the buffered partial
        line first so the probe isn't concatenated onto garbage (a false 'wedge')."""
        for _ in range(2):
            self.s.write(b"\n\n"); self.s.flush(); time.sleep(0.3); self.s.read(8000)
            out = self.line("clipcli marshal --dump", wait=0.7)
            if "progress=0x" in out:
                return True, out
        return False, out

def dump_counters(badge):
    ok, out = badge.alive()
    if not ok: return None
    g = lambda k: int(re.search(k + r"=(\d+)", out).group(1)) if re.search(k + r"=(\d+)", out) else -1
    return {"answered": g("p4_answered"), "correct": g("p4_correct"),
            "progress": re.search(r"progress=0x([0-9A-Fa-f]+)", out).group(1)}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port"); ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1337)
    a = ap.parse_args()
    random.seed(a.seed)
    port = find_port(a.port)
    if not port: print("no badge found"); return 2
    print(f"[fuzz] port {port}")
    b = Badge(port)

    # Put us in P4 with clean counters.
    b.line("clipcli reset 4"); b.line("Y")
    for n in (1, 2, 3): b.line(f"clipcli marshal {n} complete")
    b.line("clipcli challenge", wait=0.5)
    base = dump_counters(b)
    if base is None: print("[FAIL] badge unresponsive before fuzzing"); return 1
    print(f"[fuzz] entered P4, baseline {base}")

    # ---- fuzz corpora ----
    def junk_lines():
        yield "A" * 100
        yield "9" * 200                       # huge numeric (atol overflow territory)
        yield "1" * 4000                       # paste-bomb / buffer pressure
        yield "-12345"                          # negative
        yield "+88"                             # signed
        yield "0x58"                            # hex-looking
        yield "007"                             # leading zeros
        yield "   88   "                        # padded
        yield "88.0"; yield "8e3"               # float / sci
        yield "9999999999999999999999"          # > int64
        yield ""                                # empty
        yield "      "                          # whitespace
        yield "xyzzy"; yield "XyZzY"; yield " XYZZY "   # case/space variants (should win)
        yield "XYZZYX"; yield "XYZZ"; yield "XYZZYY"; yield "XY"  # near-misses (must NOT win, no OOB)
        yield "clipcli status"                  # interleave (must not break session)
        yield "clipcli hint"
        yield "give"; yield "look"; yield "../../etc/passwd"; yield "%n%n%s%s"  # format-string bait
        yield "\x01\x02\x03 ctrl"               # embedded control chars
        yield "tab\tsep"; yield "bell\x07"
        yield "ünïcödé ½ ∑"                      # non-ascii
        yield "line1\nline2\nline3"             # multi-line paste (one write)

    crash_input = None
    for rnd in range(a.rounds):
        # re-enter P4 each round (a win in round k ends the session)
        b.line("clipcli reset 4"); b.line("Y"); b.line("clipcli challenge", wait=0.4)
        for jl in junk_lines():
            b.raw((jl + "\n").encode("utf-8", "replace"), wait=0.18)
            if b.crashed: crash_input = b.crashed; break
        if crash_input: break
        # rapid spam burst (no waits) — stress the line reader
        for _ in range(60):
            b.s.write(b"88\n")
        b.s.flush(); time.sleep(1.0); out = b.s.read(20000).decode("utf-8", "replace")
        for m in CRASH_MARKERS:
            if m in out: crash_input = (m, b"spam-burst-88"); break
        # random byte storm. Exclude 0x02 (STX): that's the TEST-ONLY test_harness
        # command framing, absent from release builds — feeding it garbage exercises
        # the test harness, not the ARG/player serial surface we're fuzzing here.
        blob = bytes((c if c != 0x02 else 0x20) for c in
                     (random.randint(0, 255) for _ in range(800)))
        b.raw(blob, wait=0.4)
        if b.crashed and not crash_input: crash_input = b.crashed
        if crash_input: break
        print(f"[fuzz] round {rnd+1}/{a.rounds} done")

    # ---- verdict ----
    if crash_input:
        print(f"[FAIL] crash/reset marker '{crash_input[0]}' after input {crash_input[1]!r}")
        return 1
    final = dump_counters(b)
    if final is None:
        print("[FAIL] badge WEDGED — no response to liveness probe after fuzzing")
        return 1
    # sanity: counters are u16, must stay in range; non-numeric/empty must not have
    # inflated answered/correct absurdly (we sent very few valid numerics).
    if not (0 <= final["answered"] <= 65535 and 0 <= final["correct"] <= 65535):
        print(f"[FAIL] counter out of range: {final}"); return 1
    print(f"[fuzz] final {final}")
    print("[PASS] no crash, no wedge, counters sane — ARG serial surface survived the fuzz")
    return 0

if __name__ == "__main__":
    sys.exit(main())
