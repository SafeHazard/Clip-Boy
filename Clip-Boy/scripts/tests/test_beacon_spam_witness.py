#!/usr/bin/env python3
"""test_beacon_spam_witness.py -- P3: does Beacon Spam actually RADIATE beacons over the air?

Two Res34rch active-TX beacon-spam tools, each witnessed independently on the kali WiFi radio
(reusing test_evil_portal_witness's proven all-channel scan + passive-beacon liveness, so the
badge's beacon channel doesn't matter):

  RICK ROLL (Beacon Spam item 3): broadcasts a KNOWN 8-SSID set ("01 Never gonna give you up" ...
    "08 and hurt you", WiFiScan.h:376). Comparative: those SSIDs are ABSENT before start and PRESENT
    (and beaconing LIVE) while running. Known strings -> non-vacuous, ambient can't fabricate them.

  AP CLONE (Beacon Spam item 2, TAT_AP): clones a SELECTED AP's SSID with FAKE BSSIDs. Select
    shipship -> a beacon advertising SSID "shipship" from a BSSID that is NOT the real shipship
    (38:2c:4a:69:1b:e0) appears while running. The fake-BSSID + real-SSID pair is the signature.

Oracle discipline: comparative (OFF baseline vs ON) + an independent kali witness; a witness that
sees nothing while the tool claims to run -> the tool did NOT radiate (real FAIL), and a preflight
that proves the witness can read the room first -> a dead witness is CANNOT-TEST, never a false FAIL.

Rig: DUT = COM5 (Res34rch -- Beacon Spam is active-TX, gated to Res34rch). Witness = kali wlan0.

  py -3 scripts/tests/test_beacon_spam_witness.py --port COM5
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness
import rf_pcap
from test_evil_portal_witness import scan_aps, passive_beacons   # proven all-channel scan + liveness

SHIPSHIP_BSSID = "38:2c:4a:69:1b:e0"
RR_NEEDLE = "Never gonna"        # distinctive substring of the Rick Roll SSID set
MIN_LIVE = 1                     # passive beacons that must be captured to call a BSSID "live"


def rr_ssids(aps):
    return {b: v for b, v in aps.items() if RR_NEEDLE.lower() in (v[0] or "").lower()}


def clone_ssids(aps):
    # SSID "shipship" from any BSSID that is NOT the real shipship AP.
    return {b: v for b, v in aps.items()
            if (v[0] or "").strip() == "shipship" and b.lower() != SHIPSHIP_BSSID.lower()}


def arm(h, cat, item, secs=6):
    h.tool_stop(); time.sleep(0.5)
    h.cmd("tool_open %d %d" % (cat, item))
    time.sleep(secs)   # let it get on the air before we scan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("BEACON SPAM WITNESS -- DUT %s, witness kali wlan0\n" % a.port)

    if not rf_pcap.preflight(chan=6):
        print("CANNOT-TEST: kali WiFi witness preflight failed -- radiation control unavailable."); return 2

    h = Harness(port=a.port)
    results = {}
    try:
        bs = h.cat_pos("Beacon Spam")
        if bs is None:
            print("CANNOT-TEST: no Beacon Spam category -- is %s a Res34rch build?" % a.port); return 2

        # ---- RICK ROLL (item 3): known SSIDs ----
        print("== Rick Roll (item 3) ==")
        h.tool_stop(); time.sleep(1)
        base = rr_ssids(scan_aps())                       # OFF baseline
        arm(h, bs, 3)                                     # ON
        on = rr_ssids(scan_aps())
        live = 0
        for b, (ssid, ch) in on.items():
            live = passive_beacons(b, ch, secs=6)
            if live >= MIN_LIVE:
                break
        h.tool_stop()
        print("  baseline 'Never gonna' SSIDs=%d | running=%d | live beacons=%d" % (len(base), len(on), live))
        if len(base) > 0:
            results["rickroll"] = ("CANNOT-TEST", "Rick Roll SSIDs present at BASELINE -- another spammer? control dirty")
        elif len(on) >= 1 and live >= MIN_LIVE:
            results["rickroll"] = ("PASS", "%d Rick Roll SSIDs on air, %d live beacons captured" % (len(on), live))
        else:
            results["rickroll"] = ("FAIL", "Rick Roll ran but no 'Never gonna' SSID witnessed radiating (on=%d live=%d)" % (len(on), live))

        # ---- AP CLONE (item 2): clone shipship with fake BSSIDs ----
        print("== AP Clone (item 2, target shipship) ==")
        h.tool_stop(); time.sleep(1)
        sel = h.ap_scan("shipship")
        if (sel or {}).get("selected", -1) < 0:
            results["apclone"] = ("CANNOT-TEST", "could not select shipship (out of range?)")
        else:
            base_c = clone_ssids(scan_aps())
            arm(h, bs, 2)
            on_c = clone_ssids(scan_aps())
            live_c = 0
            for b, (ssid, ch) in on_c.items():
                live_c = passive_beacons(b, ch, secs=6)
                if live_c >= MIN_LIVE:
                    break
            h.tool_stop()
            print("  baseline clone-BSSIDs=%d | running=%d | live beacons=%d" % (len(base_c), len(on_c), live_c))
            if len(on_c) >= 1 and live_c >= MIN_LIVE:
                results["apclone"] = ("PASS", "%d fake-BSSID 'shipship' beacons on air, %d live" % (len(on_c), live_c))
            else:
                results["apclone"] = ("FAIL", "AP Clone ran but no fake-BSSID 'shipship' beacon witnessed (on=%d live=%d)" % (len(on_c), live_c))
    finally:
        try: h.tool_stop(); h.close()
        except Exception: pass

    print()
    for k, (v, d) in results.items():
        print("  %-9s %-11s %s" % (k, v, d))
    fails = [k for k, (v, _) in results.items() if v == "FAIL"]
    passes = [k for k, (v, _) in results.items() if v == "PASS"]
    cants = [k for k, (v, _) in results.items() if v == "CANNOT-TEST"]
    print("\n%d PASS, %d FAIL, %d CANNOT-TEST" % (len(passes), len(fails), len(cants)))
    if fails:
        return 1
    if not passes:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
