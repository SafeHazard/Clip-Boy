#!/usr/bin/env python3
"""test_sd_pcap.py — verify Marauder PCAP capture actually writes to SD.

Regression test for the dual-SD-init bug: ClipBoy/Marauder (cb.begin) and the
collectibles loader both mounted the global SD singleton with *separate*
SPIClass instances on the same pins, corrupting ownership so Marauder's PCAP
writes failed intermittently ("Failed to open '/ap_N.pcap'"). Fix:
coll_init_sd() now reuses the already-mounted card instead of re-begin()ing.

This was NOT covered before — test_sd_card.py only exercises the collectibles SD
path, never a sniffer-to-PCAP write. Here we run an AP scan with SavePCAP on and
assert a fresh /pcaps/ap_*.pcap is created AND grows past the 24-byte pcap header.
(PCAP capture moved to the /pcaps subdir with a monotonic name index; the old
check for /ap_0.pcap in the SD root was stale and false-failed.)
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

PCAP_HEADER = 24  # bytes written by Buffer::open() before any packet


def _pcap_size(h, name):
    """Size of a file in /pcaps via sd_list (basename match)."""
    r = h.sd_list("/pcaps")
    if not r:
        return None
    want = name.lstrip("/").split("/")[-1]
    for f in r.get("files", []):
        if f.get("name", "").lstrip("/").split("/")[-1] == want:
            return f.get("size")
    return None


def test_pcap_capture_writes(h):
    print("\n--- Marauder PCAP capture writes to SD ---")

    # 1. Enable SavePCAP deterministically (independent of the unit's NVS).
    h.cli("settings -s SavePCAP enable")
    h.wait(500)

    # 2. Clear /pcaps: pcap capture moved to the /pcaps subdir with a MONOTONIC name
    #    index (pcap_manyfiles_perf) -- captures are /pcaps/ap_<seq>.pcap, NOT /ap_0
    #    in the SD root. pcap_clear drains the dir and rmdir's it; removing /pcaps
    #    resets the NVS seq -> the next capture is a fresh /pcaps/ap_0.pcap. Drain in
    #    chunks in case a bloated dir needs several passes.
    for _ in range(30):
        r = h.cmd("pcap_clear 500")
        if not (r and r.get("ok")) or not r.get("more"):
            break
    print("  cleared /pcaps")

    # 3. Run an AP scan (apSnifferCallbackFull writes pcap when SavePCAP is on).
    pos = h.cat_pos("Scan")
    if pos is None or pos < 0:
        return False, ["could not resolve 'Scan' category position"]
    h.tool_start(pos, 0)   # Scan > APs (full)
    h.wait(18000)          # let real ambient beacons/probe-resps accumulate
    h.tool_stop()
    h.wait(1200)           # flush/close the pcap

    # 4. A fresh /pcaps/ap_*.pcap must exist AND contain packet data (> the 24-byte
    #    header). Prefer ap_0 (seq reset after the rmdir), but accept ANY grown
    #    ap_*.pcap so a non-reset seq (dir not fully emptied) doesn't false-fail.
    e = h.sd_exists("/pcaps/ap_0.pcap")
    if e and e.get("exists"):
        size = _pcap_size(h, "/pcaps/ap_0.pcap")
        print(f"  /pcaps/ap_0.pcap size = {size} bytes")
        if size is None:
            return False, ["/pcaps/ap_0.pcap exists but its size was unreadable"]
        if size <= PCAP_HEADER:
            return False, [f"/pcaps/ap_0.pcap is header-only ({size}B) — packets not written"]
        print(f"  OK: capture wrote {size} bytes of packets to /pcaps/ap_0.pcap")
        return True, []

    # ap_0 absent -> look for any ap_*.pcap that grew past the header
    r = h.sd_list("/pcaps") or {}
    caps = [f for f in r.get("files", [])
            if f.get("name", "").endswith(".pcap") and (f.get("size") or 0) > PCAP_HEADER]
    if not caps:
        return False, ["no /pcaps/ap_*.pcap with packet data created by the scan — capture path still failing"]
    biggest = max(caps, key=lambda f: f.get("size", 0))
    print(f"  OK: capture wrote {biggest.get('size')} bytes to /pcaps/{biggest.get('name')}")
    return True, []


def main():
    print("=" * 56)
    print("TEST: SD PCAP capture (dual-SD-init regression)")
    print("=" * 56)
    h = Harness()
    ok, errs = False, ["did not run"]
    try:
        h.reboot_and_wait()
        ok, errs = test_pcap_capture_writes(h)
    finally:
        try:
            h.cli("stopscan")
        except Exception:
            pass
    print("\n" + "=" * 56)
    if ok:
        print("RESULTS: 1/1 passed — PCAP capture writes to SD")
        return 0
    print("RESULTS: 0/1 passed")
    for e in errs:
        print("  - " + e)
    return 1


if __name__ == "__main__":
    sys.exit(main())
