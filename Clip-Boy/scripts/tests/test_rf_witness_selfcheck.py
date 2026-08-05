#!/usr/bin/env python3
"""test_rf_witness_selfcheck.py -- prove the WITNESS before it is ever pointed at a badge.

kalipi injects, kali witnesses. NO BADGE IS INVOLVED. The point is that kalipi can be
COMMANDED to emit an exact signature on an exact channel, and a badge cannot -- so ground
truth has to come from the side we control. If the witness were developed against a badge,
"witness bug" and "badge bug" would be the same observation.

THE ORACLE (two arms, four assertions):

    arm 1  emit DECOY only:   ours == 0        decoy >= N
    arm 2  emit OURS  only:   ours >= N        decoy == 0

The second arm exists because `ours == 0` alone is UNFALSIFIABLE -- nothing proves that
filter CAN return non-zero, so a typo'd field or a wrong address column reads 0 forever
and the suite goes green. Both filters are built by rf_pcap._filter() from ONE template
with only the nonce substituted, so they cannot diverge.

`generic` (any deauth, any address) is printed as a DIAGNOSTIC and never asserted: it is
a strict superset of `decoy` and cannot go red while the others are green.

  py -3 scripts/tests/test_rf_witness_selfcheck.py
  RF_DEBUG=1 py -3 scripts/tests/test_rf_witness_selfcheck.py     # marker trace
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rf_pcap
import tool_suite as TS

CH = 6
SECS = 12
PCAP = "/tmp/rf_selfcheck.pcap"
N_MIN = 50               # pre-registered BEFORE the run; emitter does ~200/s

# Per-run nonces. Locally-administered 02: -- belong to nobody, disrupt nothing.
RUN = str(int(time.time()) % 100000)
OURS = "02:cb:a0:%02x:%02x:%02x" % (int(RUN[-2:]) % 256, 0xA1, 0x01)
DECOY = "02:cb:b0:%02x:%02x:%02x" % (int(RUN[-2:]) % 256, 0xB2, 0x02)

# ── FORECAST (ritual step 4b) — written from the PLAN, before running, never from
# tracing the code. A forecast traced from the implementation agrees with its bugs.
# Order matters; counts matter; blocks that must NOT appear are named.
#
# ⚠ CORRECTED after the first run, and the correction went to the FORECAST, not the code.
# v1 of this list predicted the three `count`s BEFORE `capture.stop`. The code stops the
# capture first, and the code is RIGHT: tcpdump buffers, so the pcap is not complete
# until SIGTERM lands -- counting a live file would race the writer and undercount.
# v1 also omitted the badfcs census entirely. Both were forecast errors. That is the
# rule working as intended (plan / code / forecast are three suspects; here it was the
# third), and it is recorded rather than quietly edited so the next reader sees that a
# MISMATCH is not automatically a code defect.
FORECAST = [
    "preflight.start", "preflight.ok",
    # arm 1 -- stop, verify the bracket, THEN parse
    "capture.start", "capture.stop", "bracket.ok", "count", "count", "count",
    # arm 2
    "capture.start", "capture.stop", "bracket.ok", "count", "count", "count",
    # badfcs diagnostic (2 counts + the census marker)
    "count", "count", "badfcs.census",
]
FORECAST_ABSENT = ["preflight.FAILED", "capture.start.FAILED", "bracket.MISSED",
                   "bracket.NO_FRAMES"]


def arm(label, emit_bssid, expect_ours, expect_decoy):
    """Emit ONE nonce, count BOTH filters. expect_* are 'zero' or 'many'."""
    print("\n-- %s (emitting %s) --" % (label, emit_bssid), flush=True)
    t0 = rf_pcap.capture_start(iface="wlan0", path=PCAP)
    if t0 is None:
        return None, "capture did not start"
    # Code ordering brackets the emit: tcpdump is confirmed running (capture_start) BEFORE
    # the emit, and stopped AFTER it. No cross-machine clock is involved -- deliberately,
    # after a 1.9 s Windows<->kali skew made a timestamp bracket CANNOT-TEST a good rig.
    res = TS.kalipi("deauth-synth", str(CH), str(SECS), "wlan1", emit_bssid,
                    timeout=SECS + 60) or {}
    sent = int(res.get("sent", 0))
    rf_pcap.capture_stop(PCAP)

    # Stimulus-landed control. A clean zero from a dead emitter reads exactly like a
    # working witness in a quiet room. (Weak on its own -- mdk4 once reported sent=1 for
    # 5345 frames -- so the paired arm is the real control.)
    if sent <= 0:
        return None, "stimulus never fired (sent=%d)" % sent

    # The capture must have run for most of the emit window (clock-independent: uses only
    # the pcap's own timestamps). A capture that died early would undercount and read as
    # "not listening" rather than "nothing sent".
    ok_br, span = rf_pcap.bracket_ok(PCAP, min_span=SECS * 0.6)
    if not ok_br:
        return None, "capture ran only %.1fs of a %ds window -- died early, not a witness verdict" % (span, SECS)

    ours = rf_pcap.count(PCAP, rf_pcap._filter("deauth", bssid=OURS))
    decoy = rf_pcap.count(PCAP, rf_pcap._filter("deauth", bssid=DECOY))
    generic = rf_pcap.count(PCAP, rf_pcap._filter("deauth"))
    if None in (ours, decoy, generic):
        return None, "tshark count failed to parse"

    def chk(name, got, want):
        if want == "zero":
            return (got == 0, "%s=%d (want 0)" % (name, got))
        return (got >= N_MIN, "%s=%d (want >=%d)" % (name, got, N_MIN))

    a_ok, a_txt = chk("ours", ours, expect_ours)
    b_ok, b_txt = chk("decoy", decoy, expect_decoy)
    detail = "sent=%d  %s  %s  [diag generic=%d]" % (sent, a_txt, b_txt, generic)
    return (a_ok and b_ok), detail


def main():
    print("RF WITNESS SELF-CHECK -- kalipi injects, kali witnesses, no badge involved")
    print("run nonce: ours=%s decoy=%s  ch%d  N_MIN=%d (pre-registered)\n"
          % (OURS, DECOY, CH, N_MIN))

    if not rf_pcap.preflight(chan=CH):
        print("\nCANNOT TEST: witness preflight failed. Zeros here would be meaningless.")
        return 2
    TS.deploy()  # push the parameterised kalipi_stim to the injector

    results = []
    ok1, d1 = arm("arm 1: emit DECOY only", DECOY, "zero", "many")
    results.append(("arm1 ours==0, decoy>=N", ok1, d1))
    ok2, d2 = arm("arm 2: emit OURS only", OURS, "many", "zero")
    results.append(("arm2 ours>=N, decoy==0", ok2, d2))

    # Diagnostic for a claim the plan labels ASSUMED rather than measured.
    good, bad = rf_pcap.badfcs_census(PCAP)
    print("\n[diag] badfcs census: good=%s bad=%s  (plan assumed iwlwifi surfaces bad FCS)"
          % (good, bad))

    # ── forecast diff (ritual step 4b) ────────────────────────────────────────────
    actual = [m for m in rf_pcap.MARKERS]
    stray = [m for m in FORECAST_ABSENT if m in actual]
    print("\n===== FORECAST vs ACTUAL =====")
    print("  forecast: %s" % " ".join(FORECAST))
    print("  actual:   %s" % " ".join(actual))
    if actual == FORECAST and not stray:
        print("  MATCH -- every decision point fired as planned, in order")
    else:
        print("  MISMATCH -- plan, code, AND forecast are all suspects.")
        if stray:
            print("    blocks forecast as ABSENT that fired: %s" % stray)

    print("\n===== SUMMARY =====")
    hard = 0
    for name, ok, d in results:
        tag = "PASS" if ok else ("CANNOT-TEST" if ok is None else "FAIL")
        if ok is False:
            hard += 1
        print("  %-26s %-12s %s" % (name, tag, d))
    if any(ok is None for _, ok, _ in results):
        print("\nVERDICT: CANNOT TEST -- rig problem, NOT a witness verdict.")
        return 2
    print("\nVERDICT: %s" % ("PASS -- witness discriminates both directions" if hard == 0
                             else "FAIL"))
    return 0 if hard == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
