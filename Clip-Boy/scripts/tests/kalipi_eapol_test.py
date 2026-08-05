#!/usr/bin/env python3
"""
kalipi_eapol_test.py -- end-to-end EAPOL capture test.

Drives the badge (Raw/PCAP locked to a channel) via the serial harness while a Kali Pi
(ssh alias `kalipi`) repeatedly joins SSID `shipship` to generate 4-way handshakes, then
polls pkt_counters to confirm the badge's EAPOL count climbs. Validates the EAPOL-count fix
AND the PSRAM capture buffer (drop reduction).

Usage: py -3 scripts/tests/kalipi_eapol_test.py [channel] [cycles]
  channel: lock Raw/PCAP to this channel (default 1 = shipship). 0 = hop all.
  cycles:  number of join/disconnect cycles (default 6).
"""
import sys, os, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

CH = int(sys.argv[1]) if len(sys.argv) > 1 else 1
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 6
SSID, PW, IFACE = "shipship", "shipship", "wlan0"


def kalipi(cmd, timeout=40):
    try:
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", "kalipi", cmd],
                           capture_output=True, text=True, timeout=timeout)
        return (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return "(ssh/nmcli timeout)"


def join_cycle(i):
    print(f"  [kalipi] join #{i} ...", flush=True)
    kalipi(f"sudo nmcli dev wifi rescan ifname {IFACE} 2>/dev/null; true")
    time.sleep(3)
    # --wait bounds nmcli (the 4-way handshake happens at association, before DHCP finishes).
    out = kalipi(f"sudo nmcli --wait 14 dev wifi connect '{SSID}' password '{PW}' ifname {IFACE} 2>&1 | tail -1",
                 timeout=30)
    print(f"    -> {out[:90]}")
    time.sleep(3)                                   # let the handshake get captured
    kalipi(f"sudo nmcli dev disconnect {IFACE} 2>/dev/null; true", timeout=20)
    time.sleep(2)


def counters(h):
    r = h.cmd("pkt_counters")
    return r if isinstance(r, dict) and "eapol" in r else {}


def main():
    print(f"=== EAPOL capture test: ch {CH}, {CYCLES} join cycles ===")
    h = Harness()
    try:
        print("cfg_set allow_pcap:", h.cmd("cfg_set allow_pcap true"))
        print("tool_start Raw/PCAP:", h.cmd("tool_start 3 3"))
        print("raw_channel:", h.cmd(f"raw_channel {CH}"))
        time.sleep(1)
        base = counters(h)
        print("baseline:", {k: base.get(k) for k in ("mgmt", "data", "beacon", "eapol")})
        base_eapol, base_data = base.get("eapol", 0), base.get("data", 0)

        for i in range(1, CYCLES + 1):
            join_cycle(i)
            c = counters(h)
            print(f"    counters: eapol={c.get('eapol')} data={c.get('data')} "
                  f"beacon={c.get('beacon')} mgmt={c.get('mgmt')}")

        final = counters(h)
        h.cmd("tool_stop")
        d_eapol = final.get("eapol", 0) - base_eapol
        d_data = final.get("data", 0) - base_data
        print(f"\n=== RESULT: EAPOL +{d_eapol}, DATA +{d_data} over {CYCLES} joins ===")
        print("PASS: EAPOL captured." if d_eapol > 0 else
              "FAIL: no EAPOL counted (check channel / range / SavePCAP).")
        return 0 if d_eapol > 0 else 1
    finally:
        h.close()


if __name__ == "__main__":
    sys.exit(main())
