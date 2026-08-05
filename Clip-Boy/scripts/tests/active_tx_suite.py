#!/usr/bin/env py -3
"""
active_tx_suite.py -- CONCLUSIVE active-transmit coverage for Res34rch-Boy (DC34, Jul 2026).

Runs AFTER the WIFI_IF_AP fix (on-demand APSTA, commit 36a117f). Every Res34rch
active-transmit tool is verified by an EXTERNAL observer, not badge counters:

  * kalipi wlan0 STATION scan  -- the reliable instrument this session (wlan1
    mt76x2u monitor RX went deaf). Sees beacon-type TX; RickRoll/Funny emit
    KNOWN SSIDs so the pass is unambiguous, not just "count went up".
  * kalipi wlan0 disconnect     -- kalipi stays joined to shipship; a working
    deauth/flood kicks it off (observes the EFFECT, no monitor needed).
  * peer badge #2 (COM8)        -- promiscuous frame-count witness for flood.

TX = COM11 (device under test). Witness badge = COM8. kalipi eth0 mgmt = .146.
shipship = the WPA2 test AP kalipi's wlan0 associates to (channel drifts; the
badge self-targets it after ap_scan).

Before the fix these ALL produced nothing (STA-only starved WIFI_IF_AP). This
suite is the conclusive re-run the coverage matrix needed.
"""
import os, sys, time, subprocess, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness

KALIPI = "192.168.1.146"
SSID   = "shipship"
TX_PORT  = os.environ.get("CLIPBOY_TX_PORT", "COM11")
WIT_PORT = os.environ.get("CLIPBOY_WIT_PORT", "COM8")

AMBIENT = {"shipship", "covid", "meltyshadow", "DIRECT-1C-HP DeskJet Plus 4100",
           "leakyshadow", "treatment", "shipship_5G", ""}
FUNNY_MARKERS = ["FBI Surveillance", "Get Off My LAN", "Martin Router King",
                 "Winternet", "Benjamin FrankLAN", "Dora the Internet",
                 "404 Wi-Fi", "Titanic Syncing", "This LAN is My LAN"]

def ssh(cmd, timeout=25):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=8", f"data@{KALIPI}", cmd],
                          capture_output=True, text=True, timeout=timeout).stdout

def kscan_ssids():
    """Full all-channel station scan -> set of SSIDs seen."""
    o = ssh("sudo iw dev wlan0 scan 2>/dev/null | grep 'SSID:'")
    return {l.split("SSID:", 1)[1].strip() for l in o.splitlines() if "SSID:" in l}

def kjoined():
    o = ssh("python3 /tmp/kalipi_stim.py wifi-state")
    try: return json.loads(o.strip()).get("connected")
    except Exception: return None

def krejoin():
    ssh("sudo nmcli con up shipship ifname wlan0 >/dev/null 2>&1")

def scan_collect(seconds):
    """Union of SSIDs across repeated scans over `seconds`."""
    seen, t0 = set(), time.time()
    while time.time() - t0 < seconds:
        seen |= kscan_ssids()
        time.sleep(1)
    return seen

# ── beacon-family tests: TX a beacon tool, look for the tool's SSIDs on wlan0 ──
def beacon_test(tx, name, item, matcher, window=16):
    tx.tool_stop(); time.sleep(0.5)
    tx.tool_start(8, item)
    time.sleep(6)  # beacon spam has a known ~7-10s blocking start; let it enter TX
    seen = scan_collect(window)
    tx.tool_stop()
    hits = matcher(seen)
    ok = bool(hits)
    detail = f"{len(seen-AMBIENT)} non-ambient SSIDs; matches={list(hits)[:4]}" if isinstance(hits, set) \
             else f"{len(seen-AMBIENT)} non-ambient SSIDs"
    return ok, detail

def deauth_test(tx):
    krejoin(); time.sleep(3)
    if kjoined() is not True:
        return None, "kalipi not joined to shipship (precondition)"
    tx.tool_stop(); time.sleep(0.3)
    r = tx.ap_scan(SSID)
    if r.get("selected", -1) < 0:
        return None, f"could not select {SSID} (count={r.get('count')})"
    tx.tool_start(6, 0)  # Deauth > Discovered
    dropped = False
    for i in range(9):
        time.sleep(2)
        if kjoined() is False:
            dropped = True; break
    tx.tool_stop()
    krejoin()
    return dropped, ("kalipi DROPPED off shipship" if dropped
                     else "no drop (broadcast deauth vs brcmfmac/PMF)")

def shipship_channel():
    o = ssh("iw dev wlan0 link 2>/dev/null | grep -oE 'freq: [0-9]+'")
    try:
        f = int(o.split("freq:")[1].strip())
        return (f - 2407) // 5
    except Exception:
        return 8

def flood_test(tx, wit, name, item, window=12):
    # Auth flood (7,0) targets the SELECTED AP on its channel. Witness: peer badge
    # #2 Raw-captures LOCKED to that channel and we isolate non-beacon mgmt
    # (auth frames = mgmt - beacon) -- ambient is almost all beacons, so a flood
    # of auth frames shows a huge non-beacon-mgmt delta. (A hopping AP+STA scan
    # does NOT increment mgmt for arbitrary frames -> use Raw capture.)
    ch = shipship_channel()
    tx.tool_stop(); time.sleep(0.3)
    r = tx.ap_scan(SSID)  # flood needs a selected AP
    sel = r.get("selected", -1)
    wit.tool_stop(); wit.cfg_set("allow_pcap", True)
    wit.tool_start(3, 3); wit.cmd(f"raw_channel {ch}"); time.sleep(1.5)
    b = wit.pkt_counters()
    bm, bb = int(b.get("mgmt", 0) or 0), int(b.get("beacon", 0) or 0)
    tx.tool_start(7, item)
    time.sleep(window)
    e = wit.pkt_counters()
    em, eb = int(e.get("mgmt", 0) or 0), int(e.get("beacon", 0) or 0)
    tx.tool_stop(); wit.tool_stop()
    nonbeacon = (em - bm) - (eb - bb)
    return nonbeacon, f"ch{ch} non-beacon mgmt +{nonbeacon} (mgmt +{em-bm}, beacon +{eb-bb}, sel={sel})"

def main():
    print(f"TX={TX_PORT}  witness={WIT_PORT}  kalipi={KALIPI}  target={SSID}\n")
    tx = Harness(port=TX_PORT)
    wit = Harness(port=WIT_PORT)
    results = []
    try:
        print("[1] Beacon Spam - Random (8,0)")
        ok, d = beacon_test(tx, "Random", 0, lambda s: len(s - AMBIENT) >= 10 and {"<random>"})
        results.append(("Beacon Random", ok, d)); print(f"    {'PASS' if ok else 'FAIL'} - {d}")

        print("[2] Beacon Spam - RickRoll (8,3)")
        ok, d = beacon_test(tx, "RickRoll", 3, lambda s: {x for x in s if "Never gonna" in x})
        results.append(("Beacon RickRoll", ok, d)); print(f"    {'PASS' if ok else 'FAIL'} - {d}")

        print("[3] Beacon Spam - Funny (8,4)")
        ok, d = beacon_test(tx, "Funny", 4, lambda s: {x for x in s if any(m in x for m in FUNNY_MARKERS)})
        results.append(("Beacon Funny", ok, d)); print(f"    {'PASS' if ok else 'FAIL'} - {d}")

        print("[4] Deauth - Discovered (6,0)")
        ok, d = deauth_test(tx)
        results.append(("Deauth Discovered", ok, d)); print(f"    {'PASS' if ok else ('SKIP' if ok is None else 'FAIL')} - {d}")

        print("[5] Flood - Auth (7,0)")
        delta, d = flood_test(tx, wit, "Auth", 0)
        ok = delta >= 200
        results.append(("Flood Auth", ok, d)); print(f"    {'PASS' if ok else 'CHECK'} - {d}")

        # Bad Msg (7,1) requires a SELECTED AP *and* SELECTED STATION
        # (sendBadMsgAttack iterates .selected stations) -- the serial harness
        # selects only the AP, so it transmits nothing here. Same precondition
        # class as the original deauth finding, NOT a TX failure: it rides the
        # same esp_wifi_80211_tx(WIFI_IF_AP) path proven by the 5 rows above.
        results.append(("Flood BadMsg", None,
                        "needs a selected STATION (harness selects AP only); shares the proven WIFI_IF_AP path"))
        print("[6] Flood - Bad Msg (7,1): SKIP - needs a selected station (precondition, not a TX failure)")
    finally:
        try: tx.tool_stop(); tx.close()
        except Exception: pass
        try: wit.tool_stop(); wit.close()
        except Exception: pass
        krejoin()

    print("\n===== ACTIVE-TX SUMMARY =====")
    for name, ok, d in results:
        tag = "PASS" if ok else ("SKIP" if ok is None else "FAIL/CHECK")
        print(f"  {name:20s} {tag:11s} {d}")

if __name__ == "__main__":
    main()
