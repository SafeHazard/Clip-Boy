#!/usr/bin/env python3
"""test_evil_portal_witness.py -- does Evil Portal actually bring up its AP?

TIER A: Evil Portal is the 7th unproven tool and a THIRD transmit path. It uses
WiFi.softAP() (EvilPortal.cpp:392) with ZERO esp_wifi_80211_tx in the whole file -- the
IDF softAP stack, not the WIFI_IF_AP raw-TX primitive the other actives share. Worse, its
WiFi.mode(WIFI_AP) routes through the wrapped esp_wifi_set_mode that no-ops once
_cb_wifi_hw_up is set -- the exact mechanism of the whole-SKU silent-TX bug
([[active_tx_apsta_fix]]). So a silent softAP failure is a live possibility, which is
precisely why this needs a witness. Res34rch-only -> COM5, cat 11 item 0.

⚠ COVERAGE NOTE (P4, 2026-07-30): "Start Custom" (cat 11 item 1) is NOT separately RF-tested by
design. Custom (`cb.startEvilPortal("custom.html")`, ui_nav.h:2340) differs from Default ONLY in the
HTML PAGE served from SD -- it brings up the SAME softAP (same SSID/BSSID path), so its RADIATION is
already covered by this Default (item 0) test. The distinct behaviour of Custom (does it serve
custom.html from SD?) is HTTP-content, not RF, and would need a client to join the portal + fetch the
page. Owner decision P4(a): radiation covered here; the HTTP-content test is deferred as separate work.

ORACLE, in two stages because a naive one is confounded:
  1. baseline (portal OFF) all-channel active scan -> the set of pre-existing BSSIDs.
  2. portal ON (running asserted) all-channel scan -> a NEW BSSID appears. Then a PASSIVE
     beacon capture on that BSSID's channel confirms it is transmitting LIVE.

⚠ WHY NOT "appears-ON / vanishes-OFF": `iw scan` returns CACHED BSS entries -- a
just-stopped softAP lingers in the cache for tens of seconds, so the vanish arm reads a
false "still there" (measured: the new AP did not clear from an OFF scan 4 s after stop).
The PASSIVE monitor capture shows only CURRENTLY-transmitting APs (no cache), so it is the
sound liveness proof; the baseline-diff supplies "it is new".

⚠ SEQUENCING TRAP, cost a run: the tool button TOGGLES -- a second tool_open STOPS it. A
scan after an accidental toggle-off reads "no AP" and looks like a silent-softAP bug but is
a state error. running==True is asserted via tool_state immediately before the ON scan.

ATTRIBUTION: the new BSSID is the badge's own ESP32 radio -- softAP MAC = the chip's STA
MAC with the locally-administered bit set (observed ce:ba:97:10:61:f0 vs the chip's
cc:ba:97:10:61:f1). Confirmed by: new in the baseline-diff AND beaconing live under
passive capture. Reported, not hardcoded.

  py -3 scripts/tests/test_evil_portal_witness.py --port COM5
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness
import rf_pcap

SETTLE = 4


def scan_aps():
    """All-channel active scan on kali wlan0 (managed), then restore monitor.
    Returns {bssid: (ssid, channel)}."""
    rc, out = rf_pcap._ssh(
        "sudo ip link set wlan0 down 2>/dev/null; sudo iw dev wlan0 set type managed 2>/dev/null; "
        "sudo ip link set wlan0 up 2>/dev/null; sleep 2; "
        "sudo iw dev wlan0 scan 2>/dev/null | grep -E '^BSS |SSID:|DS Parameter set|primary channel'; "
        "sudo ip link set wlan0 down; sudo iw dev wlan0 set type monitor; "
        "sudo ip link set wlan0 up 2>/dev/null", timeout=60)
    aps, cur = {}, None
    for line in out.splitlines():
        s = line.strip()
        m = re.match(r"BSS ([0-9a-f:]{17})", s)
        if m:
            cur = m.group(1); aps[cur] = ["", None]
        elif cur and s.startswith("SSID:"):
            aps[cur][0] = s.split("SSID:", 1)[1].strip()
        elif cur and ("channel" in s):
            cm = re.search(r"channel[: ]+(\d+)", s)
            if cm and aps[cur][1] is None:
                aps[cur][1] = int(cm.group(1))
    return {b: tuple(v) for b, v in aps.items()}


def passive_beacons(bssid, channel, secs=6):
    """Monitor-mode beacon count for one BSSID on one channel. NO scan cache -- only APs
    transmitting RIGHT NOW appear. This is the sound liveness proof."""
    ch = channel or 6
    rc, out = rf_pcap._ssh(
        "sudo iw dev wlan0 set channel %d 2>/dev/null; "
        "sudo timeout %d tcpdump -i wlan0 -e -n type mgt subtype beacon and ether src %s 2>/dev/null | wc -l"
        % (ch, secs, bssid), timeout=secs + 20)
    for line in out.splitlines():
        if line.strip().isdigit():
            return int(line.strip())
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("EVIL PORTAL WITNESS -- badge %s (softAP third path) vs kali active scan\n" % a.port)

    # Preflight on ch6, NOT the softAP's likely ch1: the beacon-liveness check needs a
    # channel that HAS ambient APs, and this room's are on 6 (ch1 reads 0 beacons on a
    # perfectly good radio). The test itself does an all-channel managed scan, so the
    # preflight channel only has to prove the radio hears the room.
    if not rf_pcap.preflight(chan=6):
        print("CANNOT TEST: witness preflight failed.")
        return 2

    h = Harness(port=a.port)
    try:
        ep = h.cat_pos("Evil Portal")
        if ep is None:
            print("CANNOT TEST: %s is not a Res34rch build (no Evil Portal)." % a.port)
            return 2

        # ⚠ AUTHORIZED TARGET = 'shipship' ONLY (owner, 2026-07-29). Evil Portal's
        # "Start Default" clones the SELECTED AP's SSID -- so we must select shipship, and
        # refuse if it is not on the air. Otherwise the badge would evil-twin whatever AP
        # happened to be selected (it was cloning meltyshadow before this gate).
        sel = h.ap_scan("shipship")
        if sel.get("selected", -1) < 0:
            print("CANNOT TEST: authorized target 'shipship' not found on the air. "
                  "Refusing to clone any other AP.")
            return 2

        # baseline OFF -- assert stopped, then scan
        h.cmd("tool_stop"); time.sleep(SETTLE)
        base = scan_aps()
        print("baseline OFF: %d APs on air" % len(base))
        if len(base) < 2:
            print("CANNOT TEST: witness scan saw <2 APs -- not scanning properly.")
            return 2

        # ON -- start, CONFIRM running before scanning (toggle trap)
        r = h.cmd("tool_open %d 0" % ep)
        if not r.get("running"):
            r = h.cmd("tool_open %d 0" % ep)     # it toggled off; toggle back on
        st = h.cmd("tool_state") or {}
        if not (r.get("running") and st.get("running")):
            print("CANNOT TEST: could not confirm the portal is running (running=%s/%s)."
                  % (r.get("running"), st.get("running")))
            return 2
        time.sleep(SETTLE)
        on = scan_aps()
        new_on = {b: v for b, v in on.items() if b not in base}
        print("ON  (running=True): %d APs, %d NEW: %s"
              % (len(on), len(new_on), {b: v[0] for b, v in new_on.items()}))
        if not new_on:
            print("\nVERDICT: FAIL -- no new AP appeared while the portal was running "
                  "(softAP may be silently failing; check the wrapped esp_wifi_set_mode path)")
            return 1

        # LIVENESS via PASSIVE capture (no iw-scan cache). Confirm >=1 new BSSID is
        # actually beaconing right now while running.
        live = {}
        for b, (ssid, ch) in new_on.items():
            n = passive_beacons(b, ch, secs=6)
            live[b] = (ssid, ch, n)
            print("  passive: %s (ssid=%r ch=%s) -> %d beacons captured live" % (b, ssid, ch, n))

        confirmed = [b for b, (_, _, n) in live.items() if n > 0]
        ok = len(confirmed) >= 1
        print("\nVERDICT: %s" % (
            "PASS -- Evil Portal softAP %s is new AND beaconing live"
            % [(b, live[b][0]) for b in confirmed] if ok else
            "FAIL -- a new BSSID appeared in the scan but NONE beacon under passive capture; "
            "could be an iw-scan-cache artifact, re-run to confirm"))
        return 0 if ok else 1
    finally:
        try:
            h.cmd("tool_stop"); h.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
