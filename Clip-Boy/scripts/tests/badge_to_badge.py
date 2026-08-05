#!/usr/bin/env py -3
"""
badge_to_badge.py -- Two-Clip-Boy RF test axis (DC34, Jul 2026).

Uses a SECOND Clip-Boy as an independent RF witness for the first, resolving
the tests that kalipi's radios could not: the badge's own TRANSMIT (Deauth /
Beacon Spam) is proven by a peer badge that receives it -- no monitor-mode
adapter, no channel-lock guesswork (both detectors channel-HOP), and the peer's
MAC is a real Espressif OUI for detect ground-truth.

  TX badge (device under test) -- default COM11 (badge #1)
  RX badge (witness/detector)  -- default COM8  (badge #2)

Both must run the --test res34rch build (active cats 6/7/8 present). Override
ports with --tx/--rx or CLIPBOY_TX_PORT / CLIPBOY_RX_PORT.

WHY THIS EXISTS (the open Tier-2 finding it closes):
  The kalipi/A6210 sniff saw ZERO deauth frames from the badge. Root cause is
  NOT a firmware defect and NOT an mt76x2u sniff failure -- WiFiScan.cpp
  sendDeauthAttack() only transmits for access points flagged .selected, and the
  bare `tool_start 6 0` serial path never selected one, so there was literally
  nothing on the air. The real UI selects an AP first (tool_info: "Select the
  AP(s) first"). This script reproduces the REAL path: ap_scan selects shipship,
  THEN deauth -> the peer badge counts the frames. Proven, not asserted.

Detectors both channel-hop (WIFI_SCAN_DEAUTH and WIFI_SCAN_AP_STA are in the
main-loop channelHop() set), so the peer catches TX on whatever channel shipship
currently sits on (it drifts; we don't hard-code 7).

Axes:
  A  Deauth TX      -- TX ap_scan+select shipship, deauthAPs; RX geiger counts deauthFrames
  B  Beacon Spam TX -- TX beacon-spam random; RX AP+STA scan counts new SSIDs
  C  Espressif OUI  -- best-effort; parked (single-channel detect + real-MAC emitter) -- see notes
"""
import argparse, os, sys, time
# The hopping ap_scan blocks the badge ~15-22s for a full 1-14 sweep; raise the
# session read deadline (inherited by the harness's bridge subprocess) so the
# late response isn't dropped as a timeout. Set BEFORE Harness spawns the bridge.
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "26")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

SSID = os.environ.get("CLIPBOY_SSID", "shipship")
WINDOW_S = float(os.environ.get("CLIPBOY_B2B_WINDOW", "12"))

def poll_max(h, reader, key, window_s, interval=0.8):
    """Poll `reader()[key]` across a window, return the max seen (robust to the
    badge's internal per-second counter housekeeping)."""
    best = 0
    t0 = time.time()
    while time.time() - t0 < window_s:
        try:
            v = int(reader().get(key, 0) or 0)
            if v > best:
                best = v
        except Exception:
            pass
        time.sleep(interval)
    return best

def axis_deauth(tx, rx):
    print("\n=== Axis A: Deauth TRANSMIT (finding resolution) ===")
    rx.tool_stop(); tx.tool_stop()
    # RX: hopping deauth sniffer (geiger). deauthSnifferCallback counts 0xA0/0xC0.
    rx.geiger_start()
    time.sleep(1.0)
    base = int(rx.pkt_counters().get("deauth", 0) or 0)
    print(f"  RX deauth baseline: {base}")

    # TX: scan + SELECT shipship (the step the failed Tier-2 test skipped)
    r = tx.ap_scan(SSID)
    sel = r.get("selected", -1)
    cnt = r.get("count", 0)
    print(f"  TX ap_scan '{SSID}': count={cnt} selected_index={sel}")
    if sel is None or sel < 0:
        print(f"  RESULT: SKIP -- '{SSID}' not visible to TX badge (is the AP on air?)")
        rx.geiger_stop()
        return None

    tx.tool_start(6, 0)  # Deauth > Discovered (deauthAPs; selected-only TX)
    print(f"  TX deauth running, witnessing {WINDOW_S:.0f}s (RX hops 1-14)...")
    peak = poll_max(rx, rx.pkt_counters, "deauth", WINDOW_S)
    delta = peak - base
    tx.tool_stop(); rx.geiger_stop()
    ok = delta >= 10
    print(f"  RX deauth peak: {peak}  delta: +{delta}")
    print(f"  RESULT: {'PASS' if ok else 'FAIL'} -- peer badge {'OBSERVED' if ok else 'did NOT observe'} the badge's deauth TX")
    return ok

def axis_beacon(tx, rx):
    print("\n=== Axis B: Beacon Spam TRANSMIT (RF path proof) ===")
    rx.tool_stop(); tx.tool_stop()
    # RX: AP+Station scan (WIFI_SCAN_AP_STA hops + counts SSIDs/beacons)
    rx.tool_start(1, 1)
    time.sleep(1.0)
    base = int(rx.detect_counts().get("ap", 0) or 0)
    print(f"  RX ap baseline: {base}")

    tx.tool_start(8, 0)  # Beacon Spam > Random (no target needed)
    # Beacon Spam transmits on a SINGLE fixed channel (WIFI_ATTACK_BEACON_SPAM
    # is not in the main-loop channelHop set), while the AP+STA witness hops
    # 1-14 -- so the peer only dwells on the spam channel ~1/14 of the time and
    # undercounts. A longer window lets more hop-alignments accumulate; any
    # clear rise above the ~3 ambient 2.4GHz APs proves TX + reception.
    win = 22.0
    print(f"  TX beacon-spam running, witnessing {win:.0f}s (single-channel TX vs hopping RX)...")
    peak = poll_max(rx, rx.detect_counts, "ap", win)
    delta = peak - base
    tx.tool_stop(); rx.tool_stop()
    ok = delta >= 8
    print(f"  RX ap peak: {peak}  delta: +{delta}")
    print(f"  RESULT: {'PASS' if ok else 'FAIL'} -- peer badge {'SAW' if ok else 'did NOT see'} the spammed APs")
    return ok

def main():
    ap = argparse.ArgumentParser(description="Two-badge RF witness tests")
    ap.add_argument("--tx", default=os.environ.get("CLIPBOY_TX_PORT", "COM11"),
                    help="Device-under-test (transmitter) port")
    ap.add_argument("--rx", default=os.environ.get("CLIPBOY_RX_PORT", "COM8"),
                    help="Witness (receiver) port")
    ap.add_argument("--axis", choices=["a", "b", "all"], default="all")
    args = ap.parse_args()

    print(f"TX (under test) = {args.tx}   RX (witness) = {args.rx}   SSID = {SSID}")
    tx = Harness(port=args.tx)
    rx = Harness(port=args.rx)
    results = {}
    try:
        if args.axis in ("a", "all"):
            results["A deauth-tx"] = axis_deauth(tx, rx)
        if args.axis in ("b", "all"):
            results["B beacon-tx"] = axis_beacon(tx, rx)
    finally:
        try: tx.tool_stop(); tx.geiger_stop()
        except Exception: pass
        try: rx.tool_stop(); rx.geiger_stop()
        except Exception: pass
        tx.close(); rx.close()

    print("\n===== SUMMARY =====")
    fails = 0
    for k, v in results.items():
        tag = "PASS" if v else ("SKIP" if v is None else "FAIL")
        if v is False: fails += 1
        print(f"  {k:16s} {tag}")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
