#!/usr/bin/env python3
"""Host-side SKU binary verification (the legally load-bearing check).

Proves that the Sn34k-Boy (default, listen-only) firmware image does NOT
contain active-research capability - neither code SYMBOLS nor user-visible
STRINGS - and that the Res34rch-Boy (--res34rch) image DOES. The legal claim is about
the binary that ships, not about what the menu happens to show, so this
inspects the compiled .elf (symbols) and .bin (strings) directly.

Usage:
    py -3 check_sku_binaries.py [--sn34k DIR] [--res34rch DIR] [--baseline]

DIR defaults to <repo>/build/sn34k and <repo>/build/res34rch (see build.sh
--build-path). --baseline just prints what's present in each (no pass/fail),
used while iterating on the gating.
"""
import argparse
import os
import subprocess
import sys

TOOLS = (r"C:\Users\data\AppData\Local\Arduino15\packages\esp32\tools"
         r"\xtensa-esp32s3-elf-gcc\esp-2021r2-patch5-8.4.0\bin")
NM = os.path.join(TOOLS, "xtensa-esp32s3-elf-nm.exe")
STRINGS = os.path.join(TOOLS, "xtensa-esp32s3-elf-strings.exe")

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Active-capability code symbols (our wrapper layer, demangled). Each is the
# entry point for a tool that transmits frames / phishes. Must be ABSENT from
# the Sn34k .elf, PRESENT in the Res34rch .elf.
ACTIVE_SYMBOLS = [
    "ClipBoyMarauder::deauthAPs", "ClipBoyMarauder::deauthManual",
    "ClipBoyMarauder::deauthStations",
    "ClipBoyMarauder::beaconSpamRandom", "ClipBoyMarauder::beaconSpamList",
    "ClipBoyMarauder::beaconSpamClone", "ClipBoyMarauder::beaconRickRoll",
    "ClipBoyMarauder::beaconFunny",
    "ClipBoyMarauder::authFlood", "ClipBoyMarauder::badMsgFlood",
    "ClipBoyMarauder::badMsgStations", "ClipBoyMarauder::sleepFlood",
    "ClipBoyMarauder::sleepStations", "ClipBoyMarauder::saeCommitFlood",
    "ClipBoyMarauder::btSpamApple", "ClipBoyMarauder::btSpamWindows",
    "ClipBoyMarauder::btSpamSamsung", "ClipBoyMarauder::btSpamGoogle",
    "ClipBoyMarauder::btSpamFlipper", "ClipBoyMarauder::btSpamAll",
    "ClipBoyMarauder::startEvilPortal",
    # NOTE: sniffEAPOL is PASSIVE capture (listen-only) and intentionally stays
    # in both SKUs; sniffPMKID (active) is unreferenced and linker-GC'd from
    # both - neither belongs in this active-symbol absence list.
    # Upstream Marauder TX engine (the code that actually transmits) - the
    # legally important set: these are the frame builders, not just our wrapper.
    "WiFiScan::sendDeauthFrame", "WiFiScan::RunSourApple",
    "WiFiScan::RunEvilPortal", "WiFiScan::startWiFiAttacks",
    "EvilPortal::startPortal", "EvilPortal::setupServer",
]

# Active-capability user-visible strings (UI labels, More-Info copy, serial
# help). Must be ABSENT from the Sn34k .bin. These are specific to active
# tools - passive mentions of "deauth"/"SAE" (sniffing) use different phrasing
# and are intentionally not listed.
ACTIVE_STRINGS = [
    "ACTIVE - DISRUPTIVE", "ACTIVE - PHISHING", "ACTIVE - TRANSMITS",
    "Commit Flood", "Auth Flood", "Bad Msg", "Sleep Flood",
    "Deauth Stations", "Deauth all clients",
    "Beacon Random", "Beacon Spam", "Beacon List", "Beacon AP Clone",
    "Rick Roll", "Funny Beacon",
    "Sour Apple", "Swiftpair", "Fast Pair popups", "BT Spam",
    "Evil Portal", "captive-portal", "credential-harvest", "phishing",
    "Active Research !",
]

# Release hygiene: debug/cheat command strings that must NEVER ship in a release
# .bin (they're enabled only by `build.sh --test` -> -DCOLL_DEBUG/-DTEST_HARNESS).
# Guards against the COLL_DEBUG-left-#define'd-ON regression the audit caught.
# Checked on BOTH SKUs since both default builds are release artifacts.
DEBUG_STRINGS = [
    "coll add", "coll reset", "coll list", "coll remove",
    "coll stage", "import result:",   # DC34-92 test-only stage/import serial helpers
    "radiolfs", "radioprog",          # RADIO_PCM_TEST spike serial cmds (must never ship)
    "[MARSHAL]", "marshal --dump",    # DC34 ARG: CLIPBOY_DEBUG marshal (handoff §8 MUST be absent from release)
    "[P3TEST]", "[P5TEST]", "[ARGTEST]",  # DC34 ARG test-only serial cmds: p3pass/p5code (answer reveals!),
    "- FULL MAP",                     #   secret, p3map (map spoiler) -- all #ifdef TEST_HARNESS, never ship
]

# Suspect-symbol scan regex - for --baseline triage of upstream frame builders.
SUSPECT_RE = ("deauth|beacon|spam|evilportal|evil_portal|sourapple|"
              "sour_apple|swiftpair|saecommit|sae_commit|badmsg|bad_msg|"
              "rickroll|rick_roll|attack")


def run(tool, *args):
    return subprocess.run([tool, *args], capture_output=True, text=True,
                          errors="replace").stdout


def nm_symbols(elf):
    out = run(NM, "-C", elf)
    return out


def bin_strings(binf):
    return run(STRINGS, "-n", "5", binf)


def find_artifacts(d):
    elf = bin = None
    for f in os.listdir(d):
        if f.endswith(".elf"):
            elf = os.path.join(d, f)
        elif f.endswith(".bin") and "partitions" not in f and "boot" not in f:
            # the main app bin is Clip-Boy.ino.bin
            if f.endswith("Clip-Boy.ino.bin") or bin is None:
                bin = os.path.join(d, f)
    return elf, bin


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sn34k", default=os.path.join(REPO, "build", "sn34k"))
    ap.add_argument("--res34rch", default=os.path.join(REPO, "build", "res34rch"))
    ap.add_argument("--baseline", action="store_true")
    args = ap.parse_args()

    sn_elf, sn_bin = find_artifacts(args.sn34k)
    l_elf, l_bin = find_artifacts(args.res34rch)
    print(f"Sn34k: {sn_elf}\n       {sn_bin}")
    print(f"Res34rch : {l_elf}\n       {l_bin}\n")

    sn_nm = nm_symbols(sn_elf)
    l_nm = nm_symbols(l_elf)
    sn_str = bin_strings(sn_bin)
    l_str = bin_strings(l_bin)

    if args.baseline:
        import re
        print("=== SUSPECT SYMBOLS (Sn34k) ===")
        for line in sn_nm.splitlines():
            if re.search(SUSPECT_RE, line, re.I):
                print("  " + line.strip())
        print("\n=== ACTIVE STRINGS present in Sn34k .bin ===")
        for s in ACTIVE_STRINGS:
            if s in sn_str:
                print(f"  PRESENT: {s!r}")
        return 0

    fails = []
    print("=== SYMBOL CHECK (active code absent from Sn34k, present in Res34rch) ===")
    for sym in ACTIVE_SYMBOLS:
        in_sn = sym in sn_nm
        in_l = sym in l_nm
        status = "OK" if (not in_sn and in_l) else "FAIL"
        if status == "FAIL":
            fails.append(f"symbol {sym}: sn34k={in_sn} res34rch={in_l}")
        if in_sn or status == "FAIL":
            print(f"  [{status}] {sym}  (sn34k={in_sn}, res34rch={in_l})")
    print(f"  ({len(ACTIVE_SYMBOLS)} active symbols checked)")

    print("\n=== STRING CHECK (active strings absent from Sn34k .bin) ===")
    for s in ACTIVE_STRINGS:
        in_sn = s in sn_str
        if in_sn:
            fails.append(f"string {s!r} present in Sn34k .bin")
            print(f"  [FAIL] {s!r} present in Sn34k")
    print(f"  ({len(ACTIVE_STRINGS)} active strings checked)")

    print("\n=== RELEASE HYGIENE (debug/cheat strings absent from BOTH release .bins) ===")
    for s in DEBUG_STRINGS:
        for label, st in (("Sn34k", sn_str), ("Res34rch", l_str)):
            if s in st:
                fails.append(f"debug string {s!r} present in {label} release .bin")
                print(f"  [FAIL] {s!r} present in {label} release .bin")
    print(f"  ({len(DEBUG_STRINGS)} debug strings checked on both SKUs)")

    print("\n" + "=" * 60)
    if fails:
        print(f"FAIL: {len(fails)} issue(s) - Sn34k binary still carries active capability:")
        for f in fails:
            print("  - " + f)
        return 1
    print("PASS: Sn34k binary is free of active-research symbols AND strings;")
    print("      Res34rch binary contains them. Build-gating is airtight.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
