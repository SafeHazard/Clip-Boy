#!/usr/bin/env python3
"""test_sae_witness.py -- does SAE Commit Flood actually transmit?

TIER A: SAE is 1 of the 7 unproven tools. It transmits via esp_wifi_80211_tx(WIFI_IF_STA)
(WiFiScan.cpp:8170) -- a DIFFERENT interface from the WIFI_IF_AP path every passing
active-TX row demonstrates (ui_nav.h:2229-2236). Nothing that has passed speaks to it.
Res34rch-only -> COM5. cat 10, item 0.

ORACLE = comparative, with a genuine NONCE. An SAE commit is an 802.11 Authentication
frame (subtype 0x0b) with authentication algorithm 3 (SAE). The badge floods them at the
SELECTED AP's BSSID on that AP's channel. Verified 2026-07-29:
    ON  (Commit Flood):  30 auth frames, ALL 30 alg==3, 22 -> target BSSID
    OFF (idle):          0 alg==3 frames, but 1115 total frames captured
The OFF total-frame count is the liveness control: it proves the witness heard the channel
during the negative arm, so "0 SAE" means "none sent", not "we were deaf".

TARGET = whatever AP kalipi's wlan0 is currently associated to. Reading the association
(SSID/channel/BSSID) makes the test self-adapting -- kalipi roams between shipship /
meltyshadow, and hardcoding either is the confound that has bitten this rig before. The
badge selects that SSID; the witness pins that channel; frames are confirmed against that
BSSID.

  py -3 scripts/tests/test_sae_witness.py --port COM5
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness
import rf_pcap

KALIPI = "192.168.1.146"     # injector/AP-reference box, user data@
WINDOW = 10
MIN_SAE = 5                  # pre-registered: SAE is EC-crypto-heavy so the rate is low
MIN_TOTAL_OFF = 100          # OFF capture must hear this many frames or it is a dead rig
PCAP = "/tmp/sae.pcap"
SAE_FILTER = "wlan.fc.type_subtype==0x0b && wlan.fixed.auth.alg==3"


# ⚠ AUTHORIZED TARGET IS 'shipship' ONLY (owner instruction 2026-07-29). Active TX at a
# REAL AP's BSSID may ONLY point at shipship; every other SSID is off-limits. This test
# targets whatever kalipi is ASSOCIATED to, so the gate is: refuse unless that is shipship.
AUTHORIZED_SSID = "shipship"


def kalipi_target():
    """(ssid, channel, bssid, note) for the AUTHORIZED target shipship, found by BEACON
    SCAN -- NOT by association. kalipi need not (and here cannot) associate to shipship;
    SAE floods commit frames at shipship's BSSID on its channel, so a scan is enough. This
    also structurally prevents ever targeting a different AP: we scan for exactly
    AUTHORIZED_SSID and refuse if it is not on the air."""
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12",
                        "data@%s" % KALIPI, "sudo iw dev wlan0 scan"],
                       capture_output=True, text=True, timeout=45).stdout
    for blk in re.split(r"(?=^BSS )", r, flags=re.M):
        m = re.search(r"SSID: (.+)", blk)
        if not (m and m.group(1).strip() == AUTHORIZED_SSID):
            continue
        bssid = (re.search(r"BSS ([0-9a-f:]{17})", blk) or [None, None])[1]
        cm = re.search(r"channel (\d+)", blk)
        fr = re.search(r"freq: (\d+)", blk)
        ch = int(cm.group(1)) if cm else ((int(fr.group(1)) - 2407) // 5 if fr else None)
        if bssid and ch:
            return (AUTHORIZED_SSID, ch, bssid, AUTHORIZED_SSID)
    return (None, None, None, "not on air")


def capture(secs):
    """Capture on kali wlan0 (already monitor, channel already set). Returns pcap path."""
    rf_pcap._ssh("sudo rm -f %s" % PCAP)
    rf_pcap._ssh("sudo sh -c 'nohup tcpdump -i wlan0 -w %s -U </dev/null >/dev/null 2>&1 & "
                 "echo $! > %s.pid'" % (PCAP, PCAP))
    time.sleep(secs)
    rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'; sleep 1" % PCAP)


def count(dfilter):
    rc, out = rf_pcap._ssh("tshark -r %s -Y '%s' 2>/dev/null | wc -l" % (PCAP, dfilter), timeout=120)
    for line in out.splitlines():
        if line.strip().isdigit():
            return int(line.strip())
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()

    ssid, ch, bssid, actual = kalipi_target()
    if not (ssid and ch and bssid):
        print("CANNOT TEST: authorized target '%s' not available -- kalipi is on '%s'. "
              "Refusing to target any other AP (owner: shipship only)." % (AUTHORIZED_SSID, actual))
        return 2
    print("SAE WITNESS -- badge %s vs kali monitor; target = %s ch%d %s\n" % (a.port, ssid, ch, bssid))

    if not rf_pcap.preflight(chan=ch):
        print("CANNOT TEST: witness preflight failed on ch%d." % ch)
        return 2
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % ch)

    h = Harness(port=a.port)
    try:
        if h.cat_pos("SAE") is None:
            print("CANNOT TEST: %s is not a Res34rch build (no SAE)." % a.port)
            return 2
        r = h.ap_scan(ssid)
        if r.get("selected", -1) < 0:
            print("CANNOT TEST: badge could not select '%s'." % ssid)
            return 2

        # OFF arm FIRST -- the control, and it must be able to come out bad.
        h.tool_stop(); time.sleep(2)
        capture(WINDOW)
        off_sae = count(SAE_FILTER)
        off_total = count("")           # empty filter = all frames = liveness control
        if off_total is None or off_total < MIN_TOTAL_OFF:
            print("CANNOT TEST: OFF capture heard only %s frames -- witness not listening."
                  % off_total)
            return 2
        print("OFF (idle): %d SAE frames, %d total frames (witness alive)" % (off_sae, off_total))

        # ON arm
        sae = h.cat_pos("SAE")
        h.cmd("tool_open %d 0" % sae)
        time.sleep(3)
        capture(WINDOW)
        on_sae = count(SAE_FILTER)
        on_targeted = count("%s && wlan.da==%s" % (SAE_FILTER, bssid))
        h.tool_stop()
        print("ON  (flood): %d SAE frames, %d directed at %s" % (on_sae, on_targeted, bssid))

        ok = (on_sae is not None and on_sae >= MIN_SAE
              and off_sae == 0
              and on_targeted is not None and on_targeted > 0)
        print("\nVERDICT: %s" % (
            "PASS -- SAE Commit radiates (ON=%d alg3 frames, %d to target; OFF=%d)"
            % (on_sae, on_targeted, off_sae) if ok else
            "FAIL -- ON=%s targeted=%s OFF=%s (gate: ON>=%d, OFF==0, targeted>0)"
            % (on_sae, on_targeted, off_sae, MIN_SAE)))
        return 0 if ok else 1
    finally:
        try:
            h.tool_stop(); h.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
