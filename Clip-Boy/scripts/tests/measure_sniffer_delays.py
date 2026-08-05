#!/usr/bin/env python3
"""measure_sniffer_delays.py -- what the sniffer delay(random(0,10)) calls ACTUALLY cost.

Replaces the RF A/B that both pre-reviewers rejected as unrunnable (no probe-flood primitive
existed, the chosen observable was frozen at 0 in the tool under test, and the negative control
could not move). This instruments the exact state a removal would change -- elapsed time inside
the call -- instead of a downstream consequence of it.

Two arms, one per LIVE per-frame site:
  probe  -> Analyze > Probes, stimulus = wifi-probeflood
  deauth -> Analyze > Deauth (same callback the Geiger uses), stimulus = kalipi deauth

The number that matters is DUTY: microseconds spent inside the delay divided by the window's
wall time. Hit counts alone say nothing about throttling.

  py -3 scripts/tests/measure_sniffer_delays.py --port COM6 --arm probe
  py -3 scripts/tests/measure_sniffer_delays.py --port COM6 --arm deauth --target-ssid <ssid>
"""
import argparse, json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "20")
from harness import Harness
import tool_suite as TS

ARMS = {
    "probe":  dict(tool=(3, 1), key="probe",  label="Analyze > Probes"),
    "deauth": dict(tool=(3, 2), key="deauth", label="Analyze > Deauth (+ Geiger)"),
}
WINDOW = 30


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_DUT", "COM6"))
    ap.add_argument("--arm", choices=sorted(ARMS), required=True)
    ap.add_argument("--secs", type=int, default=WINDOW)
    args = ap.parse_args()
    arm = ARMS[args.arm]

    try:
        TS.deploy()
    except Exception as ex:
        print("deploy warn:", ex, flush=True)

    h = Harness(port=args.port, skip_boot=True)
    try:
        h.cmd("onboarding_accept")
        h.cmd("cfg_set airplane false")

        # Precondition: the command must exist, or we are measuring a firmware that
        # predates the instrumentation and every later number is meaningless.
        probe0 = h.cmd("delay_stats")
        if not probe0 or "probe_hits" not in str(probe0):
            print("CANNOT TEST: firmware has no delay_stats command.")
            return 2

        h.cmd("tool_open %d %d" % arm["tool"])
        time.sleep(1.5)
        h.cmd("delay_stats reset")
        t0 = time.time()

        if args.arm == "probe":
            res = TS.kalipi("wifi-probeflood", "30", str(args.secs), "6")
            landed = int((res or {}).get("sent", 0)) > 0
        else:
            # Signature is deauth(iface, chan, secs) -- passing only a duration would land it
            # in the IFACE slot and silently run mdk4 against an interface named "30".
            # And its ok= is returned UNCONDITIONALLY, so `sent` is the only real control:
            # mdk4's own "Packets sent: N".
            res = TS.kalipi("deauth", "wlan1", "6", str(args.secs))
            landed = int((res or {}).get("sent", 0)) > 0
        if not landed:
            # Without this, a low duty reads as "the delays are cheap" when the truth may be
            # "nothing was ever transmitted". Same shape that fooled the flock rig.
            print("CANNOT TEST: stimulus did not land: %s" % json.dumps(res or {})[:200])
            return 2

        elapsed = time.time() - t0
        st = h.cmd("delay_stats") or {}
        if isinstance(st, str):
            print("raw:", st)
            return 2

        hits = int(st.get("%s_hits" % arm["key"], 0))
        us = int(st.get("%s_us" % arm["key"], 0))
        per = int(st.get("%s_us_per_hit" % arm["key"], 0))
        duty = (us / 1e6) / elapsed * 100.0 if elapsed > 0 else 0.0

        print("arm            : %s" % arm["label"])
        print("window         : %.1f s   stimulus landed: yes" % elapsed)
        print("delay hits     : %d" % hits)
        print("time in delay  : %.3f s  (%d us)" % (us / 1e6, us))
        print("us per hit     : %d" % per)
        print("DUTY CYCLE     : %.3f %% of wall time spent inside delay()" % duty)
        if hits == 0:
            print("\nNOTE: zero hits -- the delayed branch never executed, so this run measures "
                  "NOTHING about the delay. Check the tool and the stimulus channel before "
                  "reading the duty as a low cost.")
        return 0
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
