#!/usr/bin/env python3
"""test_deauth_channel_persist.py -- NVS round-trip + the status-bar mode indicator.

TWO things that the channel-policy feature rests on and that the RF test does not touch:

1. PERSISTENCE. cfg.deauth_chan is stored in NVS, and the owner's decision to persist it
   was made on the explicit condition that the mode is always VISIBLE. So both halves get
   asserted here: the value survives a reboot, AND the radio is actually configured from it
   on the next boot -- not just the widget. A setting that reloads into the UI but not into
   the radio is the failure this feature was redesigned to avoid.

2. THE STATUS-BAR INDICATOR, which is the visibility half. It must show the mode while a
   deauth-family scan runs, from a screen OTHER than the one carrying the selector --
   because a user who starts the Geiger and walks away sees only the status bar.

Every check is paired with its opposite where one exists: hop-all must NOT add a mode
suffix (or the "always shows the mode" assertion is satisfied by a string that always says
something), and a non-default value must survive where the default would be indistinguishable
from a failed load.

  py -3 scripts/tests/test_deauth_channel_persist.py --port COM6
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "25")
from harness import Harness

HOP_ALL, TRI = 0, 200
PROBE_CH = 9          # deliberately NOT the default and NOT in 1/6/11: a stale read or a
                      # failed load falls back to 200, which this value cannot be confused with


def check(results, name, ok, detail):
    results.append((name, ok, detail))
    print("  [%s] %-34s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_DUT", "COM6"))
    args = ap.parse_args()
    results = []

    h = Harness(port=args.port, skip_boot=True)
    try:
        h.cmd("onboarding_accept")
        h.cmd("cfg_set airplane false")

        st = h.cmd("deauth_channel")
        if not (st and "mode" in st):
            print("VERDICT: CANNOT TEST -- no deauth_channel command on this firmware")
            return 2

        # ---- 1. set a distinctive value, reboot, confirm it survived --------------------
        h.cmd("deauth_channel %d" % PROBE_CH)
        st = h.cmd("deauth_channel") or {}
        check(results, "set takes effect pre-reboot", st.get("mode") == PROBE_CH,
              "mode=%s" % st.get("mode"))

        h.cmd("reboot")
        h.close()
        time.sleep(12)
        h = Harness(port=args.port, skip_boot=True)
        h.cmd("onboarding_accept")

        st = h.cmd("deauth_channel") or {}
        check(results, "value survives reboot (NVS)", st.get("mode") == PROBE_CH,
              "mode=%s after reboot" % st.get("mode"))
        # The half that matters: the RADIO layer must have been configured from NVS at boot,
        # not merely the widget. radio_mode is read back from WiFiScan, not from cfg.
        check(results, "radio configured from NVS at boot", st.get("radio_mode") == PROBE_CH,
              "radio_mode=%s" % st.get("radio_mode"))

        # ---- 2. status bar carries the mode, from another screen -----------------------
        h.cmd("nav 0 2")
        h.cmd("geiger_start")
        time.sleep(2)
        h.cmd("nav 0 0")                      # away from Radiation -- the whole point
        time.sleep(2)
        txt = h.cmd("text") or {}
        blob = str(txt)
        check(results, "status bar shows mode off-page", ("ch%d" % PROBE_CH) in blob,
              "looking for 'ch%d' from STATS>Status" % PROBE_CH)

        # Paired opposite: hop-all must NOT print a channel suffix, otherwise "it shows the
        # mode" would be satisfied by a label that always says something.
        h.cmd("deauth_channel %d" % HOP_ALL)
        time.sleep(2)
        blob2 = str(h.cmd("text") or {})
        # PAIRED, not merely absent. "ch9 is gone" is also what a blank status bar, a stopped
        # tool, or a label that quit updating produces for free -- so require the POSITIVE
        # form ("1-14", from cb_deauth_mode_short(0)) in the same read.
        check(results, "hop-all shows no ch suffix", ("ch%d" % PROBE_CH) not in blob2,
              "'ch%d' correctly absent under hop-all" % PROBE_CH)
        check(results, "hop-all shows its own token", "1-14" in blob2,
              "'1-14' present, so the absence above is meaningful")

        h.cmd("geiger_stop")
    finally:
        try:
            h.cmd("geiger_stop")
            h.cmd("deauth_channel %d" % TRI)   # restore the shipping default
        except Exception:
            pass
        h.close()

    print()
    bad = [n for n, ok, _ in results if not ok]
    print("VERDICT: %s (%d/%d)" % ("PASS" if not bad else "FAIL",
                                   len(results) - len(bad), len(results)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
