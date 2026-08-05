#!/usr/bin/env py -3
"""
station_deauth_test.py -- directed station-targeted deauth (DC34, Jul 2026).

Verifies the badge can deauth ONE specific client on a known channel. Uses the
`deauth_sta <client_mac> <ap_bssid> <chan>` harness cmd, which drives
deauthManual's sendDeauthFrame(src_mac, set_channel, dst_mac) directly --
bypassing the AP-list targeted deauth (6,2), whose duplicate wrong-channel AP
entry made it miss.

Rig: TX badge COM11, kalipi wlan0 associated to shipship (the victim client).
Requires kalipi's client MAC to be stable -- Kali NetworkManager randomizes it
by default, so pin it:
  nmcli con modify shipship wifi.cloned-mac-address <hw_mac>

Reads the target's MAC + shipship's BSSID/channel live from kalipi, fires the
directed deauth, and checks kalipi drops off shipship.
"""
import os, sys, time, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "40")
from harness import Harness

KALIPI = "192.168.1.146"
PORT   = os.environ.get("CLIPBOY_TX_PORT", "COM11")

def ssh(cmd, t=40):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=8", f"data@{KALIPI}", cmd],
                          capture_output=True, text=True, timeout=t).stdout

def main():
    ssh("sudo nmcli con up shipship ifname wlan0 >/dev/null 2>&1"); time.sleep(3)
    mac = ssh("cat /sys/class/net/wlan0/address").strip()
    link = ssh("iw dev wlan0 link 2>/dev/null")
    bm = re.search(r"Connected to ([0-9a-f:]{17})", link)
    fm = re.search(r"freq: (\d+)", link)
    if not (bm and fm):
        print("SKIP: kalipi not associated to shipship"); return 2
    bssid = bm.group(1)
    chan = (int(fm.group(1)) - 2407) // 5
    print(f"target client = {mac}   shipship = {bssid} ch{chan}")

    h = Harness(port=PORT)
    dropped = False
    try:
        h.tool_stop(); time.sleep(0.5)
        r = h.cmd(f"deauth_sta {mac} {bssid} {chan}")
        if not r.get("ok"):
            print("FAIL: deauth_sta rejected:", r); return 1
        print("deauth_sta:", {k: r.get(k) for k in ("client", "bssid", "chan")})
        for i in range(12):
            time.sleep(2)
            if "shipship" not in ssh("iw dev wlan0 link 2>/dev/null"):
                dropped = True; print(f"  dropped at t={2*(i+1)}s"); break
    finally:
        h.tool_stop(); h.close()
        ssh("sudo nmcli con up shipship ifname wlan0 >/dev/null 2>&1")
    print("RESULT:", "PASS - directed station deauth dropped the client" if dropped else "FAIL - no drop")
    return 0 if dropped else 1

if __name__ == "__main__":
    sys.exit(main())
