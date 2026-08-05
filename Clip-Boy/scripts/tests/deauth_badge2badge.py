#!/usr/bin/env py -3
"""Badge-to-badge deauth DETECT test — no kalipi, no target AP needed.

COM11 (res34rch) transmits directed deauth frames (deauth_sta) at a FABRICATED,
non-existent target (locally-administered 02:.. MACs, so no real network is
disrupted), sweeping 2.4GHz channels. COM8 (sn34k) runs its deauth detector
(geiger) and we watch pkt_counters.deauthFrames climb. This proves the badge's
deauth DETECTOR end-to-end using the badge's own deauth TX as the stimulus,
bypassing kalipi's dead mt76x2u injection + the flapping shipship AP.

  py -3 scripts/tests/deauth_badge2badge.py [--tx COM11] [--rx COM8]

Both --test builds. Fabricated targets only.
"""
import argparse, sys, time, re
import serial

STX = b"\x02"
FAKE_CLIENT = "02:00:00:00:00:02"   # locally-administered, non-existent
FAKE_BSSID  = "02:00:00:00:00:01"

def open_badge(port):
    s = serial.Serial(); s.port = port; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0.3; s.write_timeout = 4
    s.open(); time.sleep(0.8); s.reset_input_buffer(); return s

def cmd(s, c, w=0.4):
    s.write(STX + c.encode() + b"\n"); s.flush(); time.sleep(w)
    return s.read(6000).decode("utf-8", "replace")

def deauth_count(s):
    for _ in range(3):
        out = cmd(s, "pkt_counters", 0.4)
        m = re.search(r'"deauth[A-Za-z]*":\s*(\d+)', out)
        if m: return int(m.group(1))
        time.sleep(0.2)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tx", default="COM11"); ap.add_argument("--rx", default="COM8")
    a = ap.parse_args()
    print(f"[deauth-b2b] TX={a.tx} (res34rch)  RX={a.rx} (sn34k detector)")
    rx = open_badge(a.rx); tx = open_badge(a.tx)

    # RX: start the deauth detector (geiger)
    g = cmd(rx, "geiger_start", 1.0)
    print("  geiger_start:", "ok" if '"ok":true' in g else g[-120:])
    time.sleep(1.0)
    base = deauth_count(rx)
    print(f"  RX baseline deauthFrames = {base}")
    if base is None:
        print("  ABORT: couldn't read RX deauth counter"); rx.close(); tx.close(); return 2

    # TX: sweep deauth across 2.4GHz channels so we hit whatever ch the RX detector sits on
    chans = [1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13]
    peak = base
    for sweep in range(3):
        for ch in chans:
            cmd(tx, f"deauth_sta {FAKE_CLIENT} {FAKE_BSSID} {ch}", 0.12)
            time.sleep(0.5)
        c = deauth_count(rx)
        if c is not None:
            peak = max(peak, c)
        print(f"  sweep {sweep+1}/3: RX deauthFrames = {c}  (peak +{peak - base})")

    # read the counter ONE more time BEFORE stopping (geiger_stop resets it to 0)
    c = deauth_count(rx)
    if c is not None: peak = max(peak, c)
    delta = peak - base
    print(f"\n  RX peak deauthFrames = {peak}  (total delta +{delta})")
    cmd(tx, "tool_stop", 0.5)
    cmd(rx, "geiger_stop", 0.5)
    rx.close(); tx.close()
    ok = delta is not None and delta >= 1
    print("[deauth-b2b] " + ("PASS — COM8 detected COM11's deauths" if ok
                             else "FAIL — RX detector saw no deauths (channel mismatch or TX not radiating?)"))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
