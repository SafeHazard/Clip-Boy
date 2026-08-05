#!/usr/bin/env python3
"""test_probe_cap.py -- exercise CB_PROBE_SSID_LIST_CAP on real hardware.

WHAT IT PROVES
  RED  arm (firmware built with -DCB_PROBE_SSID_LIST_CAP=0, i.e. the pre-fix behaviour):
       the probe-SSID list grows without bound toward the number of distinct SSIDs injected.
  GREEN arm (firmware built with a LOW cap, e.g. -DCB_PROBE_SSID_LIST_CAP=10):
       the list pins at exactly the cap, AND `probe_reqs` keeps CLIMBING -- which is the
       half the badge's Help and README actually promise the user ("the counts beside names
       already listed keep rising"). A cap that froze the tool would satisfy the first
       assertion and fail the user; only the pair distinguishes them.

WHY BOTH ARMS ARE REQUIRED
  A capped list and a tool that has stopped processing probes look identical on the size
  alone -- both sit at the cap. And a low count means nothing unless we have shown, on the
  same rig, that the count CAN go high. So neither arm is a result by itself.

RUN IT (one arm per firmware build; the cap is compile-time):
  CB_EXTRA_DEFS='-DCB_PROBE_SSID_LIST_CAP=0'  bash scripts/build.sh --test
  bash scripts/reliable_flash.sh COM6
  py -3 scripts/tests/test_probe_cap.py --port COM6 --arm red   --expect-cap 0

  CB_EXTRA_DEFS='-DCB_PROBE_SSID_LIST_CAP=10' bash scripts/build.sh --test
  bash scripts/reliable_flash.sh COM6
  py -3 scripts/tests/test_probe_cap.py --port COM6 --arm green --expect-cap 10

⚠ The cap is a COMPILE-TIME constant. This script cannot set it and deliberately does not
  try to infer it -- it asserts what you tell it to expect and reports CANNOT TEST when the
  preconditions for that expectation are not met.
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "20")
from harness import Harness
import tool_suite as TS

ANALYZE_PROBES = (3, 1)      # Tools > Analyze > Probes
NAMES = 30                   # distinct SSIDs injected (CBPRB00..CBPRB29)
BURST = 20                   # seconds per burst
CHAN = 6


def counts(h):
    d = h.detect_counts() or {}
    return int(d.get("probe", -1)), int(d.get("probe_reqs", -1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_DUT", "COM6"))
    ap.add_argument("--arm", choices=["red", "green"], required=True)
    ap.add_argument("--expect-cap", type=int, required=True,
                    help="0 = uncapped (RED baseline); N = the compiled cap (GREEN)")
    args = ap.parse_args()

    verdict, notes = "CANNOT TEST", []

    def note(s):
        notes.append(s)
        print("   " + s, flush=True)

    try:
        TS.deploy()
    except Exception as ex:
        print("deploy warn:", ex, flush=True)

    h = Harness(port=args.port, skip_boot=True)
    try:
        # ---- preconditions. Bail loudly rather than emit a vacuous pass. -------------
        h.cmd("onboarding_accept")
        h.cmd("cfg_set airplane false")

        p0, r0 = counts(h)
        if p0 < 0:
            note("CANNOT TEST: detect_counts has no 'probe'/'probe_reqs' field -- "
                 "firmware predates this test.")
            print("VERDICT: CANNOT TEST"); return 2

        h.cmd("tool_open %d %d" % ANALYZE_PROBES)
        time.sleep(2.0)
        p_start, r_start = counts(h)
        # RunProbeScan clears the list at scan start, so a large value here means the tool
        # did not actually start -- and every later reading would be about a stale list.
        if p_start > 5:
            note("CANNOT TEST: list did not clear at tool start (probe=%d) -- "
                 "tool likely not running." % p_start)
            print("VERDICT: CANNOT TEST"); return 2
        note("precondition OK: tool open, probe=%d probe_reqs=%d" % (p_start, r_start))

        # ---- stimulus burst 1 --------------------------------------------------------
        res = TS.kalipi("wifi-probeflood", str(NAMES), str(BURST), str(CHAN))
        sent = int((res or {}).get("sent", 0))
        if sent <= 0:
            # THE control. Without it, "the badge saw nothing" and "nothing was transmitted"
            # are the same reading -- this exact zero already fooled us once on the flock rig.
            note("CANNOT TEST: emitter reported sent=%d -- no stimulus reached the air." % sent)
            print("VERDICT: CANNOT TEST"); return 2
        note("stimulus landed: emitter sent %d frames (%d distinct SSIDs)" % (sent, NAMES))

        p1, r1 = counts(h)
        note("after burst 1: probe=%d probe_reqs=%d" % (p1, r1))

        if p1 <= p_start:
            note("CANNOT TEST: badge recorded no probe SSIDs at all (probe %d -> %d). "
                 "Frames were emitted but not decoded -- rig/channel problem, not a cap result."
                 % (p_start, p1))
            print("VERDICT: CANNOT TEST"); return 2

        # ---- stimulus burst 2: the two-reading control -------------------------------
        # Only a SECOND burst can separate "full and still counting" from "stopped".
        res2 = TS.kalipi("wifi-probeflood", str(NAMES), str(BURST), str(CHAN))
        if int((res2 or {}).get("sent", 0)) <= 0:
            note("CANNOT TEST: second burst emitted nothing.")
            print("VERDICT: CANNOT TEST"); return 2
        p2, r2 = counts(h)
        note("after burst 2: probe=%d probe_reqs=%d" % (p2, r2))

        # ---- verdict ------------------------------------------------------------------
        if args.arm == "red":
            # Uncapped: the list should climb well past any low cap we would later impose.
            if p2 >= 20:
                verdict = "PASS"
                note("RED: list grew to %d of %d injected -- unbounded growth reproduced." % (p2, NAMES))
            else:
                verdict = "FAIL"
                note("RED: expected the list to exceed 20, got %d. Either the injection is too "
                     "weak to reproduce the defect, or a cap is compiled in after all." % p2)
        else:
            cap = args.expect_cap
            at_cap = (p2 == cap)
            still_counting = (r2 > r1)
            if at_cap and still_counting:
                verdict = "PASS"
                note("GREEN: pinned at the cap (%d) AND still counting (probe_reqs %d -> %d)."
                     % (cap, r1, r2))
            elif at_cap and not still_counting:
                verdict = "FAIL"
                note("GREEN: pinned at %d but probe_reqs did NOT rise (%d -> %d) -- the cap "
                     "froze the tool instead of just refusing new names. The Help and README "
                     "both promise the counts keep rising." % (cap, r1, r2))
            else:
                verdict = "FAIL"
                note("GREEN: expected exactly %d entries, got %d." % (cap, p2))

        print("VERDICT: %s" % verdict)
        return 0 if verdict == "PASS" else 1
    finally:
        try:
            h.tool_stop()
        except Exception:
            pass
        h.close()
        try:
            TS.kalipi_restore()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
