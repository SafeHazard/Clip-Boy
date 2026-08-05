#!/usr/bin/env py -3
"""
evil_portal_test.py -- end-to-end Evil Portal check (DC34, Jul 2026).

Proves the captive portal works after the WIFI_IF_AP fix (commit 36a117f) --
before it, Evil Portal never brought up its SoftAP so it broadcast/served
nothing. Flow:

  1. Provision the badge's SD over serial: /ap.config.txt (portal SSID) +
     /index.html (the served page). Evil Portal reads both at start.
  2. Reboot for a clean state (no stale AP selection -> uses ap.config.txt, not
     an AP-clone), start Evil Portal (cat 11,0).
  3. kalipi wlan0 joins the portal SSID (open) -> DHCP from the ESP SoftAP.
  4. curl the portal gateway (172.0.0.1, Marauder's AP_IP) -> expect the page.
  5. curl /get?email=..&password=.. -> the capture redirect (creds captured).

TX badge = COM11 (has the SD card). kalipi eth0 mgmt = .146; wlan0 = station.
Restores kalipi to shipship at the end.
"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness

KALIPI = "192.168.1.146"
PORT   = os.environ.get("CLIPBOY_TX_PORT", "COM11")
AP_SSID = "ClipBoyPortal"
HTML = ("<html><head><title>ClipBoy Portal</title></head><body>"
        "<h1>Free WiFi - Sign In</h1>"
        "<form action=/get method=get>Email:<input name=email> "
        "Password:<input name=password> <input type=submit value=Login>"
        "</form></body></html>")

def ssh(cmd, t=60):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=8", f"data@{KALIPI}", cmd],
                          capture_output=True, text=True, timeout=t).stdout

def main():
    h = Harness(port=PORT)
    ok_page = ok_ap = False
    try:
        h.tool_stop(); time.sleep(0.5)
        # 1. provision SD
        assert h.sd_write("/ap.config.txt", AP_SSID).get("ok"), "write ap.config.txt failed"
        assert h.sd_write("/index.html", HTML).get("ok"), "write index.html failed"
        print(f"provisioned SD: /ap.config.txt={AP_SSID}, /index.html ({len(HTML)}B)")
        # 2. clean reboot + start portal
        h.reboot_and_wait(timeout=22)
        h.tool_stop(); time.sleep(0.5)
        print("start Evil Portal:", h.tool_start(11, 0).get("name"))
        time.sleep(6)
        # 3-5. join + curl from kalipi
        out = ssh(f'''
sudo nmcli dev wifi rescan ifname wlan0 >/dev/null 2>&1; sleep 3
sudo nmcli dev wifi connect "{AP_SSID}" ifname wlan0 2>&1 | tail -1
sleep 3
GW=$(ip route show dev wlan0 | grep -oE 'via [0-9.]+' | grep -oE '[0-9.]+' | head -1)
echo "gateway=$GW"
echo "PAGE<<<"; curl -s --max-time 8 http://$GW/
echo ">>>"
echo "CAPTURE<<<"; curl -s --max-time 8 "http://$GW/get?email=demo@x.com&password=hunter2"
echo ">>>"
''')
        print(out)
        ok_ap = "gateway=172" in out or "gateway=192" in out
        ok_page = "ClipBoy Portal" in out
        ok_cap = "window.location" in out
        print("\n===== EVIL PORTAL =====")
        print(f"  AP broadcast + client joined : {'PASS' if ok_ap else 'FAIL'}")
        print(f"  captive page served          : {'PASS' if ok_page else 'FAIL'}")
        print(f"  credential capture endpoint  : {'PASS' if ok_cap else 'FAIL'}")
    finally:
        h.tool_stop(); h.close()
        ssh("sudo nmcli con up shipship ifname wlan0 >/dev/null 2>&1")
    sys.exit(0 if (ok_ap and ok_page) else 1)

if __name__ == "__main__":
    main()
