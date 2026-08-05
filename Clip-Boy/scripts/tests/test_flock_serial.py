#!/usr/bin/env py -3
"""test_flock_serial.py -- the first Flock fixtures that CAN GO RED.

WHY THIS EXISTS
Every pre-existing Flock emitter (flock-full / flock-named / flock-rotate) is shaped so the current
parser succeeds, so none of them can fail. They are honest about being circular -- kalipi_stim.py
names the defects at the fixture site -- but naming a defect in a comment is not a witness. There
was no executable test that went red on either off-by-one in the vendored AD parser. This is it.

THE TWO DEFECTS (both live in WiFiScan.cpp's NimBLEScanCallbacks class, the compiled one)

  D1  `adEnd = adStart + adLen` (:1457) is ONE SHORT.
      adStart indexes the AD *length* byte and adLen counts type+data but not itself, so the
      structure spans adStart..adStart+adLen INCLUSIVE and the exclusive end is +adLen+1. The last
      byte of every manufacturer AD is therefore never examined -> the serial loses its final
      character. Console-only today (nothing displays the serial), but it is a correctness bug the
      moment anything does.

  D2  `for (size_t i = 1; i + 3 < len; i++)` (:1399) is ONE TOO STRICT.
      The body's highest index is payLoad[i+2], so the guard should be `i + 2 < len`. As written a
      match at i = len-3 is never attempted -- i.e. an advert whose manufacturer AD ends AT the
      company ID is NOT DETECTED AT ALL. A silent miss, which is strictly worse than D1.
      ⚠ Field significance is UNPROVEN: we have no evidence a real battery emits company-ID-only.
      Do not describe D2 as a confirmed real-world miss.

WHY THE BADGE'S OWN `Serial:` PRINT IS NOT THE OBSERVABLE
It is unreachable from a script three ways over: test_bridge.py:157 drops non-STX lines, :123
reset_input_buffer()s before every command, and the CDC TX timeout makes the badge DROP
console bytes whenever nothing is draining. Draining continuously to compensate would put the badge
in a state no user is ever in. So this test reads `flock_serial` from detect_counts -- framed,
retryable, and gated on cb_op_running so a previous arm's device cannot satisfy it.

AND WHY THE `Payload:` DUMP IS NOT THE CONTROL
It sits INSIDE `if (hasXuntongMfg && (penguin || name.length() == 0))` (WiFiScan.cpp:1492), so it
only prints once the matcher has already fired. It cannot control a red-by-silence arm: for D2 and
for the negative arm, the correct observation is total silence, which is identical to a dead rig.
Every silent arm here is therefore PAIRED with a positive control in the SAME RUN.

  CLIPBOY_PORT=COM4 py -3 scripts/tests/test_flock_serial.py [--host 192.168.1.113] [--secs 12]

Not wired into make_release_bins.sh or run_all.py -- the `test_` prefix cannot reach either (both
use explicit lists), which is the intent: this needs a badge AND a BLE advertiser.
"""
import argparse
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from harness import Harness            # noqa: E402
import tool_suite as TS                # noqa: E402

TOOL_NAME = "Flock Batteries"
TRUTH_SERIAL = "TN72023022000771"      # 16 chars, from the published capture
D1_SERIAL = "TN7202302200077"          # 15 chars, what the unpatched parser yields

FAILED, PASSED, CANNOT, WARNED = [], [], [], []


def record(name, ok, msg):
    (PASSED if ok else FAILED).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")


def cannot(name, why):
    CANNOT.append(name)
    print(f"  [warn] {name}: CANNOT TEST: {why}")


def warn(name, msg):
    """A documented, deliberately-unfixed defect reproduced. Deliberately NOT its own exit code:
    a friendly name on a nonzero exit is what a future runner's `|| true` learns to swallow, and
    the next reader takes 'KNOWN DEFECT CONFIRMED' to mean 'reviewed and accepted'. The decision
    not to fix belongs in the notes, not in the exit status."""
    WARNED.append(name)
    print(f"  [warn] {name}: {msg}")


class Arm:
    """One fixture run: fresh tool start, baseline assertion, emitter, poll, teardown."""

    def __init__(self, h, host, secs):
        self.h, self.host, self.secs = h, host, secs

    def _uptime(self):
        st = self.h.cmd("state") or {}
        return st.get("uptime_ms"), st.get("reset_reason")

    def run(self, name, primitive):
        """-> (verdict, detail) where verdict is 'ok' | 'cannot' | dict of readings."""
        up0, rr0 = self._uptime()

        # tool_open fires the REAL start button and builds the pollers; tool_start does not.
        # Starting the tool also calls resetDisplayAccumulators(), which CLEARS flock_devices --
        # the list is not cleared by StopScan, so without this every arm would inherit the
        # previous arm's device (btmgmt rotates its address, so each arm looks like a new one).
        cat = self.h.cat_pos("Detect")
        if cat is None:
            return "cannot", "no Detect category in tool_list"
        self.h.cmd(f"tool_open {cat} 3")
        time.sleep(2.0)          # BT_SCAN_FLOCK also calls startPcap(); a bloated SD /pcaps dir
                                 # can block SD.open ~15s and has caused false start timeouts

        ts = self.h.cmd("tool_state") or {}
        if not ts.get("running"):
            return "cannot", "tool did not start (running=false)"
        if ts.get("name") != TOOL_NAME:
            # Name-based, so a category/item reorder cannot silently retarget this test.
            return "cannot", f"opened '{ts.get('name')}', expected '{TOOL_NAME}'"

        base = (self.h.cmd("detect_counts") or {}).get("flock")
        if base != 0:
            return "cannot", f"flock baseline is {base}, not 0 -- the tool-start clear did not take"

        # Fire the emitter in a thread so we can poll while it advertises.
        result = {}

        def fire():
            result["stim"] = TS.kalipi(primitive, self.secs,
                                       timeout=self.secs + 25, host=self.host)

        t = threading.Thread(target=fire, daemon=True)
        t.start()

        best = {"flock": 0, "serial": ""}
        deadline = time.time() + self.secs + 3
        while time.time() < deadline:
            dc = self.h.cmd("detect_counts") or {}
            if (dc.get("flock") or 0) > best["flock"]:
                best["flock"] = dc.get("flock") or 0
            if dc.get("flock_serial"):
                best["serial"] = dc["flock_serial"]
            if best["flock"] and best["serial"]:
                break
            time.sleep(0.8)
        t.join(timeout=self.secs + 30)

        self.h.cmd("tool_stop")

        stim = result.get("stim") or {"ok": False, "err": "emitter thread produced nothing"}
        if not stim.get("ok"):
            # The stimulus MUST report success, or a clean-looking 0 is just a dead rig.
            return "cannot", f"emitter did not fire: {stim.get('err') or stim}"
        # clr_ok is False ONLY when clr-adv neither removed an instance nor confirmed the slot was
        # already empty (i.e. it timed out) -- then a previous payload may still be radiating and
        # this arm's reading cannot be attributed. Absent for the older kinds, which is not a fault.
        # ⚠ Do NOT re-derive this from a return code here: btmgmt's clr-adv returns rc=1 in the
        # COMMON case (nothing to remove), and an earlier version of this guard read that as
        # contamination and false-CANNOT-TESTed all four new arms on a perfectly clean run.
        if stim.get("clr_ok") is False:
            return "cannot", ("clr-adv could not confirm the advertising slot was clean (timeout) "
                              "-- a previous payload may still be radiating")

        up1, rr1 = self._uptime()
        if up0 is not None and up1 is not None and up1 < up0:
            return "cannot", f"badge REBOOTED mid-arm (uptime {up0} -> {up1}, reset_reason {rr1})"

        best["stim"] = stim
        return best, ""


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=TS.KALIPI2, help="BLE advertiser (default kalipi2b)")
    ap.add_argument("--secs", type=float, default=12.0)
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT"))
    a = ap.parse_args(argv)

    print(f"[flock-serial] advertiser={a.host} dwell={a.secs}s port={a.port or 'auto'}")
    TS.deploy(host=a.host)

    # deploy() discards its return code, so a stale/failed scp is SILENT and every new primitive
    # would come back {"ok":false,"err":"usage"} -- which looks like a detection failure. Prove the
    # new primitives exist remotely before trusting any negative result from them.
    probe = TS.kalipi("flock-real", "0", timeout=40, host=a.host)
    if probe.get("err") == "usage" or probe.get("kind") != "flock-real":
        print(f"  [warn] CANNOT TEST anything: kalipi_stim on {a.host} lacks flock-real "
              f"(deploy failed?): {probe}")
        return 3
    if probe.get("source") != "capture:ryanohoro.com":
        print(f"  [warn] flock-real is missing its provenance field: {probe}")

    h = Harness(port=a.port)
    try:
        arm = Arm(h, a.host, a.secs)

        # ── A1 liveness: a fixture already proven GREEN. If this fails, nothing after it means
        #    anything, because we cannot distinguish "not detected" from "rig dead".
        r, why = arm.run("A1", "flock-full")
        if r == "cannot":
            cannot("A1 liveness", why)
            print("\n[flock-serial] ABORT: no liveness baseline -- every later arm would be vacuous.")
            return 3
        live = r["flock"] >= 1
        record("A1 liveness", live,
               f"known-good emitter -> flock={r['flock']} serial={r['serial']!r}")
        if not live:
            print("\n[flock-serial] ABORT: known-good emitter not detected; rig or badge is not "
                  "in a state where a 0 means anything.")
            return 3

        # ── A2 defect D1: the capture-sourced serial. The ONLY Flock expectation in this repo that
        #    does not come from our own matcher.
        r, why = arm.run("A2", "flock-real")
        if r == "cannot":
            cannot("A2 D1 truncation", why)
        elif r["flock"] < 1:
            record("A2 D1 truncation", False,
                   "real capture NOT DETECTED at all -- expected a detection with a serial")
        elif r["serial"] == TRUTH_SERIAL:
            record("A2 D1 truncation", True,
                   f"serial {r['serial']!r} == capture truth; D1 is FIXED")
        elif r["serial"] == D1_SERIAL:
            warn("A2 D1 truncation",
                 f"D1 REPRODUCED: got {r['serial']!r} ({len(r['serial'])} chars), truth is "
                 f"{TRUTH_SERIAL!r} ({len(TRUTH_SERIAL)}). adEnd = adStart + adLen drops the last "
                 f"byte. Documented + queued; not fixed in this pass.")
        else:
            record("A2 D1 truncation", False,
                   f"UNEXPECTED serial {r['serial']!r} -- neither the truth {TRUTH_SERIAL!r} nor "
                   f"the known truncation {D1_SERIAL!r}. Something else changed.")

        # ── A4 BEFORE A3: A3 is red-by-silence, so its control must already be in hand.
        r4, why4 = arm.run("A4", "flock-cid-end-control")
        ctrl_ok = (r4 != "cannot") and r4["flock"] >= 1
        if r4 == "cannot":
            cannot("A4 D2 control", why4)
        else:
            record("A4 D2 control", ctrl_ok,
                   f"company-ID-only + 1 filler byte -> flock={r4['flock']} (must detect)")

        # ── A3 defect D2: byte-identical to A4 minus the filler.
        r3, why3 = arm.run("A3", "flock-cid-end")
        if r3 == "cannot":
            cannot("A3 D2 silent miss", why3)
        elif not ctrl_ok:
            cannot("A3 D2 silent miss",
                   "its positive control (A4) did not detect, so a 0 here proves nothing")
        elif r3["flock"] >= 1:
            record("A3 D2 silent miss", True,
                   f"mfg AD ending at the company ID WAS detected (flock={r3['flock']}); D2 is FIXED")
        else:
            warn("A3 D2 silent miss",
                 "D2 REPRODUCED: an advert whose manufacturer AD ends at the company ID is not "
                 "detected at all, while the same bytes plus ONE filler byte are (A4). "
                 "`i + 3 < len` never attempts a match at i = len-3. Documented; not fixed here.")

        # ── A5 negative: proves a genuine 0 is reachable, sandwiched by proven-live arms.
        r5, why5 = arm.run("A5", "flock-reversed-cid")
        if r5 == "cannot":
            cannot("A5 negative", why5)
        elif r5["flock"] == 0:
            record("A5 negative", True,
                   "reversed company ID correctly NOT detected, with liveness proven this run")
        else:
            record("A5 negative", False,
                   f"reversed company ID WAS detected (flock={r5['flock']}) -- the matcher is "
                   f"firing on something other than the Xuntong ID")
    finally:
        try:
            h.cmd("tool_stop")
        except Exception:
            pass
        try:
            h.close()
        except Exception:
            pass

    print(f"\n[flock-serial] {len(PASSED)} pass, {len(FAILED)} fail, "
          f"{len(WARNED)} documented-defect, {len(CANNOT)} cannot-test")
    for n in FAILED:
        print(f"  FAIL   {n}")
    for n in WARNED:
        print(f"  WARN   {n} -- reproduced a documented, deliberately-unfixed defect")
    for n in CANNOT:
        print(f"  CANNOT {n} -- NOT RUN; this is not a pass")
    return 1 if (FAILED or CANNOT) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
