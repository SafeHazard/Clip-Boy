#!/usr/bin/env py -3
"""
ep_littlefs_fallback_test.py -- prove the Evil Portal LittleFS fallback (commit
31c100a). Marauder ships Evil Portal SD-only; we added a built-in template
fallback at LittleFS /examples/evil_portal/{index.html,ap.config.txt} so a badge
flashed from the web-flasher with NO SD card can still serve the example portal.

A/B, unambiguous by SSID + page content (each source serves a DIFFERENT page):

  COM8  (NO SD card)      -> must serve the BUILT-IN example  -> LittleFS worked.
        SSID Example-Portal-EDIT-ME, page contains "EXAMPLE TEMPLATE".
  COM11 (SD card present) -> provision a CUSTOM page on SD; must serve THAT, not
        the LittleFS example -> proves SD stays PRIMARY (fallback only on miss).
        SSID ClipBoyPortal-SD, page contains "SD-PRIMARY-PAGE".

kalipi eth0 mgmt = .146; wlan0 = station that joins each portal and curls it.
Restores kalipi to shipship at the end.

NOTE: the definitive no-SD proof is the on-badge serial (both templates read
"from LittleFS", "ap ip address: 172.0.0.1") cross-checked with a kalipi curl of
the EXAMPLE page -- verified 2026-07-11 on commit 31c100a. This script automates
the happy path; the no-SD case can still flake on the reboot_and_wait->start
window (nmcli scan-cache timing), so treat a case-A FAIL as "re-run", not a
firmware regression, unless the badge serial shows "Could not find".
"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness

KALIPI    = "192.168.1.146"
PORT_NOSD = os.environ.get("CLIPBOY_NOSD_PORT", "COM8")
PORT_SD   = os.environ.get("CLIPBOY_SD_PORT",   "COM11")

LFS_SSID  = "Example-Portal-EDIT-ME"      # built-in ap.config.txt
LFS_MARK  = "EXAMPLE TEMPLATE"            # built-in index.html marker

SD_SSID   = "ClipBoyPortal-SD"
SD_MARK   = "SD-PRIMARY-PAGE"
SD_HTML   = ("<html><head><title>SD Primary</title></head><body>"
             f"<h1>{SD_MARK}</h1><p>served from the SD card</p></body></html>")


def ssh(cmd, t=90):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=8", f"data@{KALIPI}", cmd],
                          capture_output=True, text=True, timeout=t).stdout


def join_and_curl(ssid):
    """kalipi wlan0 joins `ssid` (open) and curls the gateway. Returns stdout.

    nmcli's `connect` fails outright if the SSID isn't in wlan0's cached scan --
    and the cache can lag a freshly-raised SoftAP by 10-15s. So POLL: rescan and
    check the scan list until the SSID appears (bounded), THEN connect. That
    removes the scan-cache race that made the no-SD case flaky.
    """
    return ssh(f'''
for i in $(seq 1 10); do
  sudo nmcli dev wifi rescan ifname wlan0 >/dev/null 2>&1
  sleep 5
  if nmcli -t -f SSID dev wifi list ifname wlan0 2>/dev/null | grep -qx "{ssid}"; then
    echo "ssid-visible-after=${{i}}"; break
  fi
done
sudo nmcli dev wifi connect "{ssid}" ifname wlan0 2>&1 | tail -1
sleep 3
GW=$(ip route show dev wlan0 | grep -oE 'via [0-9.]+' | grep -oE '[0-9.]+' | head -1)
echo "gateway=$GW"
echo "PAGE<<<"; curl -s --max-time 8 http://$GW/ ; echo ">>>"
sudo nmcli dev disconnect wlan0 >/dev/null 2>&1
''', t=180)


def run_case(label, port, ssid, expect_mark, provision=None):
    print(f"\n===== {label} (port {port}, SSID {ssid}) =====")
    h = Harness(port=port)
    served = False
    joined = False
    try:
        h.tool_stop(); time.sleep(0.5)
        if provision:
            for p, c in provision:
                r = h.sd_write(p, c)
                assert r.get("ok"), f"sd_write {p} failed: {r}"
                print(f"  provisioned SD {p} ({len(c)}B)")
        # NOTE: do NOT probe sd_exists on the no-SD badge -- SD.open on a card-less
        # board blocks for seconds (retry/cardType) and desyncs the session.
        h.reboot_and_wait(timeout=22)
        time.sleep(4)  # extra settle: the no-SD badge needs a beat past reboot_and_wait
        h.tool_stop(); time.sleep(0.5)
        print("  start Evil Portal ->", h.tool_start(11, 0).get("name"))
        time.sleep(14)  # let the SoftAP stabilize (no-SD path is a touch slower)
        out = join_and_curl(ssid)
        print(out.strip())
        joined = "gateway=172" in out or "gateway=192" in out
        served = expect_mark in out
        if not served:  # one retry -- nmcli scan cache can lag a fresh SoftAP
            print("  (retrying join -- scan cache may have lagged)")
            out = join_and_curl(ssid)
            print(out.strip())
            joined = joined or "gateway=172" in out or "gateway=192" in out
            served = expect_mark in out
    finally:
        h.tool_stop(); h.close()
    print(f"  client joined AP : {'PASS' if joined else 'FAIL'}")
    print(f"  served '{expect_mark}' : {'PASS' if served else 'FAIL'}")
    return joined and served


def main():
    results = {}
    try:
        # A: no-SD badge must fall back to the built-in LittleFS template.
        results["LittleFS fallback (COM8 no-SD)"] = run_case(
            "A / LittleFS FALLBACK", PORT_NOSD, LFS_SSID, LFS_MARK)
        # B: SD badge must serve the SD page, NOT the LittleFS example.
        results["SD primary (COM11 SD)"] = run_case(
            "B / SD PRIMARY", PORT_SD, SD_SSID, SD_MARK,
            provision=[("/ap.config.txt", SD_SSID), ("/index.html", SD_HTML)])
    finally:
        ssh("sudo nmcli con up shipship ifname wlan0 >/dev/null 2>&1")
    print("\n=========== SUMMARY ===========")
    for k, v in results.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    sys.exit(0 if all(results.values()) else 1)


if __name__ == "__main__":
    main()
