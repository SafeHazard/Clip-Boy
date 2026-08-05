#!/usr/bin/env python3
"""test_deauth_channel.py -- prove the deauth channel policy actually steers the radio.

THREE ARMS, and all three are needed:

  baseline  hop 1-14,  flood ch1  -> LOW   (badge is on ch1 ~1/14 of the time)
  positive  lock ch1,  flood ch1  -> HIGH  (continuous listening)
  negative  lock ch11, flood ch1  -> ~AMBIENT

The NEGATIVE arm is the one that makes this a test rather than a demo. Without it,
"locking increased the count" is equally explained by a broken lock that merely stopped
visiting quiet channels. And it uses ch1 vs ch11 deliberately: 802.11 channels are 5 MHz
apart with 20 MHz of occupied bandwidth, so a badge on ch5 hears a ch6 flood perfectly
well. 1 and 11 are the standard non-overlapping pair -- an adjacent pair would let the
negative arm read high and prove nothing.

The BASELINE runs FIRST and must come out low. A control that could only ever come out
good is not a control.

Direct observable, not just a packet delta: `live` is read back from esp_wifi_get_channel()
via the deauth_channel harness command, because changeChannel() discards
esp_wifi_set_channel()'s return value -- so our own set_channel is bookkeeping, not
evidence that the radio moved.

  py -3 scripts/tests/test_deauth_channel.py --port COM6
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "20")
from harness import Harness
import tool_suite as TS

FLOOD_CH = 1
SECS = 30
HOP_ALL, TRI = 0, 200


def arm(h, label, mode, expect_live):
    """Set the mode, restart the scan so it re-asserts; returns (delta, live_ch, err_or_None)."""
    h.cmd("geiger_stop")
    st = h.cmd("deauth_channel %d" % mode)
    if not (st and st.get("ok")):
        return None, None, "could not set mode %d" % mode
    if not h.cmd("geiger_start"):
        return None, None, "geiger_start failed"
    time.sleep(2)
    st = h.cmd("deauth_channel") or {}
    live = st.get("live")
    if expect_live is not None and live != expect_live:
        # The lock did not take. Report it rather than measuring a badge that is
        # somewhere other than where the test believes it is.
        return None, live, "radio on ch%s, expected ch%s" % (live, expect_live)

    pc = h.pkt_counters() or {}
    # The JSON key is "deauth" (wifi_scan_obj.deauth_frames). The C++ FIELD is deauthFrames,
    # and using that name here made every arm read 0 -- a completely vacuous test that only
    # looked like a real FAIL. Assert the key EXISTS rather than .get()-ing a default, so a
    # renamed or missing counter reports CANNOT-TEST instead of a confident zero.
    if "deauth" not in pc:
        return None, live, "pkt_counters has no 'deauth' key: %s" % sorted(pc)[:8]
    before = pc["deauth"]
    # deauth-synth, NOT mdk4: amok mode only blasts APs it finds on the channel, so on a
    # channel with no APs it sends nothing (measured: sent=0, empty output on ch1). It also
    # deauths REAL neighbours; the synthetic emitter uses fabricated 02: addresses.
    res = TS.kalipi("deauth-synth", str(FLOOD_CH), str(SECS))
    sent = int((res or {}).get("sent", 0))
    if sent <= 0:
        return None, live, "stimulus did not land (sent=%d)" % sent
    time.sleep(1)
    pc2 = h.pkt_counters() or {}
    # Strict on BOTH reads. Hardening only the pre-read left this one defaulting to 0, which
    # would make delta NEGATIVE on a failed read -- and the negative arm asserts
    # `neg < max(base,1)`, so a broken read would PASS. Same bug, opposite end.
    if "deauth" not in pc2:
        return None, live, "post-flood pkt_counters lost the 'deauth' key"
    delta = pc2["deauth"] - before
    print("  %-9s mode=%-4s live=ch%-3s sent=%-6d deauth_delta=%d"
          % (label, mode, live, sent, delta), flush=True)
    return delta, live, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_DUT", "COM6"))
    args = ap.parse_args()
    try:
        TS.deploy()
    except Exception as ex:
        print("deploy warn:", ex, flush=True)

    h = Harness(port=args.port, skip_boot=True)
    try:
        h.cmd("onboarding_accept")
        h.cmd("cfg_set airplane false")
        h.cmd("nav 0 2")

        base, _, err = arm(h, "baseline", HOP_ALL, None)
        if err:
            print("VERDICT: CANNOT TEST (baseline): %s" % err); return 2
        pos, _, err = arm(h, "positive", FLOOD_CH, FLOOD_CH)
        if err:
            print("VERDICT: CANNOT TEST (positive): %s" % err); return 2
        neg, _, err = arm(h, "negative", 11, 11)
        if err:
            print("VERDICT: CANNOT TEST (negative): %s" % err); return 2

        print()
        # The docstring claims the baseline 'must come out low'. Nothing ASSERTED it, and a
        # low baseline and a DEAF baseline are indistinguishable: base==0 collapsed the gate
        # to 'pos>0 and neg==0' and printed PASS -- while base==0 is exactly the hop-all
        # regression this test is supposed to catch.
        if base <= 0:
            print("VERDICT: CANNOT TEST -- baseline read 0; hop-all heard nothing, so there is"
                  " no reference for the locked arms (deaf badge or dead stimulus).")
            return 2
        ok_pos = pos > base * 2
        ok_neg = neg < max(base, 1)
        print("  positive > 2x baseline : %-5s (%d vs %d)" % (ok_pos, pos, base))
        print("  negative < baseline    : %-5s (%d vs %d)" % (ok_neg, neg, base))
        verdict = "PASS" if (ok_pos and ok_neg) else "FAIL"
        print("VERDICT: %s" % verdict)
        return 0 if verdict == "PASS" else 1
    finally:
        try:
            h.cmd("geiger_stop")
            h.cmd("deauth_channel %d" % TRI)   # leave the badge on the shipping default
        except Exception:
            pass
        h.close()
        try:
            # kalipi_restore() only brings wlan0 back -- deauth-synth put wlan1 into
            # monitor mode and nothing else takes it out, so every run would leave the
            # injector parked there for whatever runs next.
            TS.kalipi("mon-down", "wlan1")
            TS.kalipi_restore()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
