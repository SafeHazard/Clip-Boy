#!/usr/bin/env python3
"""test_flood_beacon_witness.py -- do Flood > Auth and Beacon Spam > Random actually RADIATE?

Two Res34rch active-TX tools that had only liveness coverage. Each gets a COMPARATIVE on-air oracle
(kali monitor witness) with an OFF control that CAN come out bad. Promoted from a diagnostic that
verified both on hardware 2026-07-30 (Flood>Auth 4504 probe-reqs ON / 0 OFF; Beacon>Random 53 distinct
SSIDs ON / 2 OFF). Rig facts: [[kali_rf_witness]], [[rf_tool_coverage_map]].

  Flood > Auth (cat "Flood", item 0): despite the name it is a PROBE-REQUEST flood -- authFlood ==
    probeFlood, both StartScan(WIFI_ATTACK_AUTH) -> sendProbeAttack (WiFiScan.cpp:9885). It sends
    probe-reqs carrying the SELECTED AP's SSID, random src MAC, on the SELECTED AP entry's channel
    (access_points.get(i).channel:9891). Oracle: probe-reqs (subtype 0x04) for the target SSID,
    ON >> OFF, aimed at the channel the BADGE actually selected (self-adapting -- also surfaces the
    station/AP-channel class: we report the selected channel, we do NOT hardcode it).
  Beacon Spam > Random (cat "Beacon Spam", item 0): broadcastRandomSSID (9837) -- beacons with a
    random 6-char SSID, random src MAC, a RANDOM channel 1-11 PER FRAME. Oracle: DISTINCT beacon
    SSIDs spike ON vs OFF on one fixed witness channel (the flood floods enough that ~1/11 landing
    on ch6 still dominates ambient).

AUTHORIZED TARGET = 'shipship' ONLY (owner, 2026-07-29): Flood>Auth clones the selected AP's SSID,
so we refuse unless shipship is on the air and selected. Beacon>Random invents random SSIDs (no real
AP touched).

Rig: DUT = COM5 (res34rch --test). Witness = kali .11 wlan0 monitor (rf_pcap). Authorized WiFi target
shipship. Emitter = the DUT itself (these are TX tools). CANNOT-TEST (rc 2) on any dead-rig condition
-- never a false FAIL.

  py -3 scripts/tests/test_flood_beacon_witness.py --port COM5
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "35")
from harness import Harness
import rf_pcap

PCAP = "/tmp/floodbeacon.pcap"
WINDOW = 10                # capture seconds per arm
AUTH_SSID = "shipship"

# Pre-registered thresholds (set BEFORE the run so a result can't be rationalized after).
MIN_OFF_TOTAL   = 30       # OFF capture must hear this many frames or the witness is deaf
AUTH_MIN_ON     = 50       # probe-flood is very fast; measured 4504 in 10s -- 50 is a safe floor
AUTH_MAX_OFF    = 5        # ambient probe-reqs for shipship should be ~0
BEACON_MIN_OFF_FRAMES = 10 # OFF must hear ambient beacons (control alive)
BEACON_MIN_DELTA = 10      # ON distinct-SSIDs must exceed OFF by this...
BEACON_MIN_RATIO = 2.0     # ...AND be >= this multiple of OFF


def _capture(secs):
    rf_pcap._ssh("sudo rm -f %s" % PCAP)
    rf_pcap._ssh("sudo sh -c 'nohup tcpdump -i wlan0 -w %s -U </dev/null >/dev/null 2>&1 & "
                 "echo $! > %s.pid'" % (PCAP, PCAP))
    time.sleep(secs)
    rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'; sleep 1" % PCAP)


def _count(dfilter):
    rc, out = rf_pcap._ssh("tshark -r %s -Y '%s' 2>/dev/null | wc -l" % (PCAP, dfilter), timeout=120)
    for line in out.splitlines():
        if line.strip().isdigit():
            return int(line.strip())
    return None


def _distinct_ssids(subtype):
    rc, out = rf_pcap._ssh(
        "tshark -r %s -Y 'wlan.fc.type_subtype==%s' -T fields -e wlan.ssid 2>/dev/null "
        "| sed '/^$/d' | sort -u | wc -l" % (PCAP, subtype), timeout=120)
    for line in out.splitlines():
        if line.strip().isdigit():
            return int(line.strip())
    return None


def _setch(ch):
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % ch)


def flood_auth_arm(h):
    """Returns True (PASS) / False (FAIL) / None (CANNOT-TEST)."""
    print("\n===== Flood > Auth (probe-request flood) =====")
    fl = h.cat_pos("Flood")
    if fl is None:
        print("CANNOT TEST: no Flood category (not a Res34rch build)."); return None
    sel = h.ap_scan(AUTH_SSID)
    if sel.get("selected", -1) < 0:
        print("CANNOT TEST: authorized target '%s' not on air / not selected." % AUTH_SSID); return None
    apl = h.ap_list(AUTH_SSID) or {}
    rows = apl.get("aps") or apl.get("list") or []
    chosen = [r for r in rows if r.get("sel")]
    ch = (chosen[0].get("ch") if chosen else (rows[0].get("ch") if rows else None))
    print("  '%s' AP entries: %s ; badge attack channel = %s"
          % (AUTH_SSID, [(r.get("bssid"), r.get("ch"), r.get("sel")) for r in rows], ch))
    if not ch:
        print("CANNOT TEST: no selected '%s' entry/channel." % AUTH_SSID); return None
    if not rf_pcap.preflight(chan=ch):
        print("CANNOT TEST: witness preflight failed on ch%d." % ch); return None
    _setch(ch)
    PF = "wlan.fc.type_subtype==0x04 && wlan.ssid==\"%s\"" % AUTH_SSID

    h.tool_stop(); time.sleep(2)
    _capture(WINDOW)
    off_pf, off_total = _count(PF), _count("")
    if off_total is None or off_total < MIN_OFF_TOTAL:
        print("CANNOT TEST: OFF heard %s frames (< %d) -- witness deaf, not a badge fault."
              % (off_total, MIN_OFF_TOTAL)); return None
    print("  OFF: %d probe-reqs for '%s', %d total frames (witness alive)" % (off_pf, AUTH_SSID, off_total))

    h.cmd("tool_open %d 0" % fl); time.sleep(3)
    _capture(WINDOW)
    on_pf = _count(PF)
    h.tool_stop()
    print("  ON : %d probe-reqs for '%s' on ch%d" % (on_pf, AUTH_SSID, ch))
    if on_pf is None:
        print("CANNOT TEST: ON capture failed."); return None
    ok = on_pf >= AUTH_MIN_ON and (off_pf or 0) <= AUTH_MAX_OFF
    print("  VERDICT: %s (gate: ON>=%d, OFF<=%d)"
          % ("PASS -- Auth/probe flood radiates" if ok else "FAIL", AUTH_MIN_ON, AUTH_MAX_OFF))
    if ch != 11:
        print("  NOTE: badge chose ch%d for a shipship target normally on ch11 -- possible "
              "station/AP-channel mis-attribution (see rf_rx_and_station_directed)." % ch)
    return ok


def beacon_random_arm(h):
    print("\n===== Beacon Spam > Random =====")
    bs = h.cat_pos("Beacon Spam")
    if bs is None:
        print("CANNOT TEST: no Beacon Spam category."); return None
    if not rf_pcap.preflight(chan=6):
        print("CANNOT TEST: witness preflight failed on ch6."); return None
    _setch(6)

    h.tool_stop(); time.sleep(2)
    _capture(WINDOW)
    off_d, off_b = _distinct_ssids("0x08"), _count("wlan.fc.type_subtype==0x08")
    if off_b is None or off_b < BEACON_MIN_OFF_FRAMES:
        print("CANNOT TEST: OFF heard %s beacons (< %d) -- witness deaf."
              % (off_b, BEACON_MIN_OFF_FRAMES)); return None
    print("  OFF: %d distinct beacon SSIDs, %d beacon frames (control alive)" % (off_d, off_b))

    h.cmd("tool_open %d 0" % bs); time.sleep(3)
    _capture(WINDOW)
    on_d = _distinct_ssids("0x08")
    h.tool_stop()
    print("  ON : %d distinct beacon SSIDs (ch6; flood hops 1-11)" % on_d)
    if on_d is None or off_d is None:
        print("CANNOT TEST: SSID count failed."); return None
    ok = on_d >= BEACON_MIN_RATIO * max(off_d, 1) and (on_d - off_d) >= BEACON_MIN_DELTA
    print("  VERDICT: %s (gate: ON >= %.0fx OFF and +%d distinct)"
          % ("PASS -- random-SSID beacon flood radiates" if ok else "FAIL",
             BEACON_MIN_RATIO, BEACON_MIN_DELTA))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("FLOOD/BEACON WITNESS -- DUT %s vs kali monitor\n" % a.port)

    h = Harness(port=a.port)
    try:
        res_a = flood_auth_arm(h)
        res_b = beacon_random_arm(h)
    finally:
        try:
            h.tool_stop(); h.close()
        except Exception:
            pass

    print("\n==== SUMMARY ====")
    print("  Flood > Auth (probe flood)   : %s" % {True: "PASS", False: "FAIL", None: "CANNOT-TEST"}[res_a])
    print("  Beacon Spam > Random         : %s" % {True: "PASS", False: "FAIL", None: "CANNOT-TEST"}[res_b])
    if res_a is None or res_b is None:
        return 2                 # any CANNOT-TEST -> rig, not a badge failure
    return 0 if (res_a and res_b) else 1


if __name__ == "__main__":
    sys.exit(main())
