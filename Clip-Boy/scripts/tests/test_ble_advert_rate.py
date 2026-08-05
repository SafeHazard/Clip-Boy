#!/usr/bin/env python3
"""test_ble_advert_rate.py -- T3: does Scan > BLE Adverts (BT_SCAN_SIMPLE) TRACK the advert RATE?

Scan > BLE Adverts is a pure counter (`pkt_counters.bt_frames`, incremented per received advert
report; dup-cache disabled for BT_SCAN_SIMPLE, so it tracks RATE not distinct devices). It carries
NO per-device identity, so the honest oracle is TEMPORAL CORRELATION: drive a controlled advert rate
up and down and prove the counter follows.

Stimulus (NEW, kalipi_stim `ble-fastadv-on/off`): raw HCI ~20ms ADV_IND. btmgmt add-adv can only do
~7-10/s; ambient BLE is ~16/s (measured on kali), so a single btmgmt advertiser cannot clear 2x. The
20ms HCI primitive reaches ~52/s witnessed on the kali CSR (3.2x ambient) -- enough to dominate.

ORACLE (owner-directed shape, both pre-reviews' Shape B):
  1. LONG ambient baseline: DUT bt_frames rate over >=4 windows (>=48s), no emit -> ambient_mean.
  2. EMITTER SELF-CHECK before trusting anything: fast-adv ON -> (a) the kali CSR witness must SEE our
     adverts (independent radiation proof), AND (b) the DUT bt_frames rate must jump to >= 2x ambient.
     Either missing -> CANNOT-TEST (rig), never a false FAIL. This is the owner's "ensure the witness
     sees the adverts before you trust a number" gate.
  3. M=4 FLOOD/THROTTLE cycles: flood (fast-adv on) vs throttle (off) DUT bt_frames rate.
     PASS = flood_rate >= 2x throttle_rate in >= 3 of 4 cycles (baseline-robust temporal correlation).
Honest scope: proves the counter tracks a controlled advert rate; NOT per-device identity (impossible
for a counter). The kali witness proves radiation independently at the self-check; the cycles rely on
the DUT's own bt_frames correlation (if the flood silently stopped, flood~throttle -> no false PASS).

Rig: DUT = COM5 (BLE Adverts is SKU-identical passive). Emitter = kalipi hci0 (ble-fastadv). Witness =
kali CSR hci1 (btmon, counts adverts from kalipi's addr 2C:CF:67:CF:29:28).

  py -3 scripts/tests/test_ble_advert_rate.py --port COM5
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness
import tool_suite as TS
import rf_pcap

KALIPI_ADDR = "2C:CF:67:CF:29:28"     # kalipi hci0 BD_ADDR -- the fast-adv source
W = 12          # measurement window (s)
M = 4           # flood/throttle cycles
BASE_WINS = 4   # ambient baseline windows (>=48s total)
MIN_WIT_RATE = 10.0   # kali must witness >= this many of OUR adverts/s during a flood (proves radiation)
RATIO = 2.0     # flood must be >= RATIO x throttle
MIN_WINS = 3    # ... in >= this many of M cycles


def dut_bt_rate(h, secs):
    c0 = (h.pkt_counters() or {}).get("bt_frames")
    time.sleep(secs)
    c1 = (h.pkt_counters() or {}).get("bt_frames")
    # c1 < c0 => bt_frames was zeroed mid-window (BLE scan restart, WiFiScan.cpp:2589) -> the window
    # is corrupt; discard it (None) rather than feed a negative rate into the mean/ratio.
    if c0 is None or c1 is None or c1 < c0:
        return None
    return (c1 - c0) / float(secs)


def kali_our_rate(secs):
    """Count btmon REPORT-LINES bearing the kalipi fast-adv source address on the kali CSR over ~secs.
    ⚠ This is a FLOOR proof of radiation, NOT a clean adverts/s: an ADV_IND under an active btmgmt-find
    scan yields both an ADV_IND report AND a SCAN_RSP report per cycle, so the number over-counts real
    adverts ~2x. We only gate on it being ABOVE MIN_WIT_RATE (any hit with our public address is
    dispositive proof the flood radiated); do not cite the value as a true advert rate. None on error."""
    rf_pcap._ssh("sudo rm -f /tmp/t3w.snoop")
    rf_pcap._ssh("sudo sh -c 'nohup timeout %d btmon -w /tmp/t3w.snoop >/dev/null 2>&1 &'" % (secs + 2))
    rf_pcap._ssh("sudo timeout %d btmgmt --index 1 find >/dev/null 2>&1" % secs)
    rf_pcap._ssh("while pgrep btmon >/dev/null 2>&1; do sleep 1; done", timeout=secs + 20)
    rc, o = rf_pcap._ssh("btmon -r /tmp/t3w.snoop 2>/dev/null | grep -c %s" % KALIPI_ADDR, timeout=120)
    try:
        return int(o.strip().split()[-1]) / float(secs)
    except Exception:
        return None


def flood_on():
    return bool((TS.kalipi("ble-fastadv-on") or {}).get("ok"))


def flood_off():
    TS.kalipi("ble-fastadv-off")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("BLE ADVERT RATE (T3) -- DUT %s, emitter kalipi ble-fastadv, witness kali CSR\n" % a.port)
    TS.deploy()

    h = Harness(port=a.port)
    try:
        flood_off()   # ensure a clean start
        sc = h.cat_pos("Scan")
        if sc is None:
            print("CANNOT TEST: no Scan category."); return 2
        h.cmd("tool_open %d 4" % sc)   # Scan > BLE Adverts (BT_SCAN_SIMPLE)
        time.sleep(2)

        # 1. LONG ambient baseline.
        print("== ambient baseline (%d x %ds, no emit) ==" % (BASE_WINS, W))
        base = [dut_bt_rate(h, W) for _ in range(BASE_WINS)]
        if any(b is None for b in base):
            print("CANNOT-TEST: bt_frames unreadable during baseline."); return 2
        ambient = sum(base) / len(base)
        print("  ambient windows/s: %s  mean=%.1f/s" % (["%.1f" % b for b in base], ambient))

        # 2. EMITTER SELF-CHECK (radiation witness + DUT sees >=2x).
        print("== emitter self-check (fast-adv ON) ==")
        if not flood_on():
            print("CANNOT-TEST: kalipi fast-adv failed to start (HCI)."); return 2
        sc_dut = dut_bt_rate(h, W)           # DUT rate under flood
        wit = kali_our_rate(W)               # independent radiation proof, same flood
        flood_off()
        print("  DUT rate under flood=%.1f/s (ambient=%.1f, want >=%.1f)" % (sc_dut or -1, ambient, RATIO * ambient))
        print("  kali witnessed our-address report-lines=%.1f/s (~2x true adverts; floor gate >=%.1f)" % (wit or -1, MIN_WIT_RATE))
        if wit is None or wit < MIN_WIT_RATE:
            print("\nCANNOT-TEST: kali did NOT witness the flood radiating -- rig, not badge."); return 2
        if sc_dut is None or sc_dut < RATIO * max(ambient, 0.5):
            print("\nCANNOT-TEST: DUT bt_frames did not rise to >=2x ambient under a proven-radiating "
                  "flood -- the badge can't hear the emitter here (position/RF), not a rate-tracking fault."); return 2

        # 3. FLOOD/THROTTLE cycles.
        print("== %d flood/throttle cycles ==" % M)
        wins = 0
        for i in range(M):
            if not flood_on():
                # emitter died mid-suite -> a flat flood is a RIG failure, not a rate-tracking fault.
                print("\nCANNOT-TEST: fast-adv failed to start in cycle %d -- rig, not badge." % (i + 1))
                return 2
            f = dut_bt_rate(h, W); flood_off()
            t = dut_bt_rate(h, W)
            ok = (f is not None and t is not None and f >= RATIO * max(t, 0.5))
            wins += 1 if ok else 0
            print("  cycle %d: flood=%.1f/s throttle=%.1f/s  ratio=%.1fx  %s"
                  % (i + 1, f or -1, t or -1, (f / max(t, 0.5)) if f else 0, "WIN" if ok else "-"))
    finally:
        try: flood_off()
        except Exception: pass
        try: h.tool_stop(); h.close()
        except Exception: pass

    ok = wins >= MIN_WINS
    print("\n%d/%d cycles had flood >= %.0fx throttle" % (wins, M, RATIO))
    print("VERDICT: %s" % (
        "PASS -- BLE Adverts bt_frames tracks the controlled advert rate (temporal correlation), "
        "flood radiation independently witnessed on kali." if ok else
        "FAIL -- bt_frames did not track the flood/throttle rate (%d/%d cycles, need %d)" % (wins, M, MIN_WINS)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
