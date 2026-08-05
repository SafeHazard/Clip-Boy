#!/usr/bin/env python3
"""test_scan_rx.py -- does the badge actually RECEIVE what is on the air?

The tier-A/B work proved the badge TRANSMITS. This proves the reverse for the passive
core -- Scan and Monitor -- which is what every one of the 152 shipped Sn34k badges runs.
Runs on either SKU (these tools are passive, present in both). Default COM4 (sn34k).

Two RX oracles, both comparative / nonce-based so ambient cannot fake a pass:

  RX1  Scan > APs receives a UNIQUE SSID.
       kalipi beacons "CBRX-<nonce>"; the badge's `ap_scan <nonce>` returns selected>=0
       ONLY if it heard that exact beacon. The NEGATIVE control is a second nonce that
       nobody transmits -> selected must be -1. That negative is what makes it a test:
       it proves the scan reports what it HEARD, not a hallucination or a stale entry.

  RX2  Monitor packet counters track offered load.
       kalipi floods DEAUTH on the badge's channel; pkt_counters.deauth must rise far
       above its idle baseline. Deauth, NOT beacons: Monitor > Packets HOPS 1-14, so a
       single-channel flood is seen ~1/14 of the time and beacons (high ambient) bury it
       -- measured, an early beacon version read on<off. Ambient DEAUTH is ~0, so even
       1/14-diluted the flood dominates. Comparative ON/OFF ratio, OFF baseline measured.

  py -3 scripts/tests/test_scan_rx.py --port COM4
"""
import argparse, os, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "45")
from harness import Harness
import tool_suite as TS
import rf_pcap

CH = 6
# deauth-synth's default fabricated BSSID. The radiation witness MUST bind to it, not count
# any deauth on the air -- ambient/neighbour deauth would otherwise let a DEAD emitter read
# "radiated" and a badge-0 be misattributed as a badge FAIL (post-review finding).
DEAUTH_BSSID = "02:cb:de:00:00:01"
EMIT_SECS = 40          # must outlast the badge's ~16 s AP-scan sweep
# Monitor > Packets HOPS 1-14 at ~1 s each = ~14 s/cycle. A 10 s window sometimes never
# visits ch6, so a single-channel flood reads 0 THROUGH NO BADGE FAULT (measured: badge=0
# while the kali radiation witness saw 3434 frames on the air). 30 s spans ~2 full cycles,
# so ch6 is caught multiple times. The radiation witness stays -- it distinguishes this
# hopping miss (witness>0 -> keep going, longer window) from a real emitter failure.
MON_SECS = 30
BEACON_RATIO = 5.0      # pre-registered: flooded deauth rate over idle baseline (ambient~0)


def rx1_scan_aps(h, nonce):
    """kalipi beacons `nonce` while the badge scans for it. Returns (heard,neg_ok,sent,count,witness)."""
    res = {}
    def emit():
        res.update(TS.kalipi("beacon-ssid", nonce, str(EMIT_SECS), str(CH),
                             timeout=EMIT_SECS + 40) or {})
    t = threading.Thread(target=emit, daemon=True)
    t.start()
    time.sleep(3)                      # let the beacon get on the air first
    # RADIATION WITNESS for RX1 too (symmetry with RX2). `sent` is emitter-side; a poisoned
    # wlan1 gives sent>0 while radiating nothing -> a false badge FAIL. kali captures the
    # nonce beacon on CH so a badge-miss with witness>0 is a real FAIL, witness~0 CANNOT-TEST.
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % CH)
    rf_pcap.capture_start(iface="wlan0", path="/tmp/rx1.pcap")
    r = h.ap_scan(nonce)               # ~16 s sweep, overlaps the emission
    heard = r.get("selected", -1) >= 0
    # NEGATIVE control -- a nonce nobody sent. Same scan machinery, must NOT select.
    neg = h.ap_scan(nonce + "X404")
    neg_ok = neg.get("selected", -1) < 0
    rf_pcap.capture_stop("/tmp/rx1.pcap")
    t.join(timeout=EMIT_SECS + 30)
    sent = int(res.get("sent", 0))
    witness = rf_pcap.count("/tmp/rx1.pcap", rf_pcap._filter("beacon", ssid=nonce)) or 0
    return heard, neg_ok, sent, r.get("count"), witness


def rx2_monitor(h, nonce):
    """Monitor packet counter vs offered DEAUTH load (ambient-immune). Returns (off,on,ratio,sent)."""
    def deauth_delta(window):
        d0 = (h.pkt_counters() or {}).get("deauth")
        time.sleep(window)
        d1 = (h.pkt_counters() or {}).get("deauth")
        return None if (d0 is None or d1 is None) else d1 - d0

    h.tool_stop(); time.sleep(1)
    mon = h.cat_pos("Monitor")
    h.cmd("tool_open %d 0" % mon)       # Monitor > Packets
    time.sleep(2)
    off = deauth_delta(MON_SECS)        # OFF baseline: ambient deauth ~= 0

    # ON: flood deauth on the badge's channel, WITH an independent radiation witness.
    # `sent` is scapy's emitter-side count and does NOT prove wlan1 radiated (measured:
    # RX2 flaked to 0 while sent=3520 -- wlan1's monitor state is intermittently bad after
    # a prior flood). So kali captures the deauth on CH in parallel: if kali heard it, the
    # flood radiated and a badge-0 is a real RX failure; if kali heard ~0, the emitter
    # failed and the result is CANNOT-TEST, not a badge verdict. This is the project's
    # "prove the stimulus landed via an independent observable" rule.
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % CH)
    rf_pcap.capture_start(iface="wlan0", path="/tmp/rx2.pcap")
    res = {}
    def emit():
        res.update(TS.kalipi("deauth-synth", str(CH), str(MON_SECS + 6), "wlan1",
                             timeout=MON_SECS + 46) or {})
    t = threading.Thread(target=emit, daemon=True); t.start()
    time.sleep(2)
    on = deauth_delta(MON_SECS)
    t.join(timeout=MON_SECS + 30)
    rf_pcap.capture_stop("/tmp/rx2.pcap")
    h.tool_stop()
    sent = int(res.get("sent", 0))
    # Bind the radiation witness to OUR fabricated BSSID (both ta and bssid) via _filter,
    # not a bare deauth-subtype count -- so ambient deauth can never make a dead emitter
    # look like it radiated. _filter also adds the badfcs==0 guard.
    witness = rf_pcap.count("/tmp/rx2.pcap", rf_pcap._filter("deauth", bssid=DEAUTH_BSSID)) or 0
    if off is None or on is None:
        return off, on, None, sent, witness
    ratio = on / max(off, 1)
    return off, on, ratio, sent, witness


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM4"))
    a = ap.parse_args()
    nonce = "CBRX%d" % (int(time.time()) % 100000)
    print("SCAN/MONITOR RX -- badge %s, nonce %s, ch%d\n" % (a.port, nonce, CH))
    TS.deploy()

    # Provision + verify the kali witness (puts wlan0 in monitor on CH). Without this the
    # RX2 radiation witness could read 0 on an unprovisioned box and silently downgrade a
    # real badge RX failure to CANNOT-TEST. Bail if the witness cannot be trusted.
    if not rf_pcap.preflight(chan=CH):
        print("CANNOT TEST: kali witness preflight failed -- radiation control unavailable.")
        return 2

    h = Harness(port=a.port)
    results = []
    try:
        # RX2 (Monitor/deauth) runs FIRST, deliberately. RX1's beacon nonce floods wlan1
        # for ~40 s; after it, a following deauth-synth reports frames `sent` but does NOT
        # radiate (the "sent != radiated" hazard -- wlan1 left in a bad monitor state),
        # so the badge's counter read 0. MEASURED: Monitor-then-Scan works; Scan-then-
        # Monitor gives a false RX2 zero. Order is load-bearing.
        off, on, ratio, sent2, witness = rx2_monitor(h, nonce)
        if ratio is None:
            results.append(("RX2 Monitor", None, "badge counter unread (off=%s on=%s)" % (off, on)))
        elif witness < 20:
            # The independent radiation witness didn't hear it -> the emitter failed, not
            # the badge. CANNOT-TEST, never FAIL.
            results.append(("RX2 Monitor", None,
                            "stimulus did NOT radiate: kali witnessed %d deauth (sent=%d) -- rig, not badge"
                            % (witness, sent2)))
        else:
            ok = on >= 20 and ratio >= BEACON_RATIO
            results.append(("RX2 Monitor", ok,
                            "deauth delta off=%d on=%d ratio=%.1fx [kali-witnessed %d radiated, sent=%d]"
                            % (off, on, ratio, witness, sent2)))
        print("  RX2 Monitor    %s" % results[-1][2])

        # RX1 (Scan/beacon nonce) -- its long emit goes LAST so it cannot poison RX2.
        heard, neg_ok, sent, count, witness = rx1_scan_aps(h, nonce)
        if witness < 20:
            # The beacon did not radiate (kali didn't hear it) -> emitter/wlan1 fault, not
            # the badge. CANNOT-TEST, never a false badge FAIL.
            results.append(("RX1 Scan>APs", None,
                            "beacon did NOT radiate: kali witnessed %d (sent=%d) -- rig, not badge"
                            % (witness, sent)))
        else:
            ok = heard and neg_ok
            results.append(("RX1 Scan>APs", ok,
                            "heard nonce=%s (count=%s), negative clean=%s [kali-witnessed %d radiated, sent=%d]"
                            % (heard, count, neg_ok, witness, sent)))
        print("  RX1 Scan>APs   %s" % results[-1][2])
    finally:
        try:
            h.tool_stop(); h.close()
        except Exception:
            pass

    print("\n===== RX SUMMARY =====")
    fails = cannot = 0
    for name, ok, d in results:
        tag = "PASS" if ok else ("CANNOT-TEST" if ok is None else "FAIL")
        cannot += ok is None
        fails += ok is False
        print("  %-14s %-12s %s" % (name, tag, d))
    if cannot:
        return 2
    print("\nVERDICT: %s" % ("PASS -- badge receives what is on the air"
                             if fails == 0 else "%d FAILED" % fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
