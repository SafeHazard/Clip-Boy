#!/usr/bin/env python3
"""Host-side IP / trademark gate (companion to check_sku_binaries.py).

Fails the build if any user-visible string in a release .bin contains a
denylisted third-party NAMED MARK or a famous verbatim line. The scrub posture
(CLAUDE.md "IP/parody posture") KEEPS franchise *references* but scrubs Bethesda
named marks + living people + verbatim copyright. This gate enforces that on
what actually SHIPS (the .bin's .rodata strings), the same way the SKU gate
enforces the listen-only split -- because the legal claim is about the binary,
not what the menu happens to show.

It intentionally lists only SPECIFIC, distinctive marks (no generic English
words like "overseer"/"wasteland"), so it never false-positives on ordinary
code or copy. Add a mark here the moment one is found in the wild.

Usage:
    py -3 check_ip.py [--bin FILE.bin] [DIR ...]
DIR defaults to build/{sn34k,res34rch,sn34k-rift,res34rch-rift}.
"""
import argparse
import os
import subprocess
import sys

TOOLS = (r"C:\Users\data\AppData\Local\Arduino15\packages\esp32\tools"
         r"\xtensa-esp32s3-elf-gcc\esp-2021r2-patch5-8.4.0\bin")
STRINGS = os.path.join(TOOLS, "xtensa-esp32s3-elf-strings.exe")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Distinctive third-party named marks / verbatim lines. Matched case-insensitively
# as substrings against the .bin strings. KEEP SPECIFIC (no generic words).
DENYLIST = [
    "robco", "termlink", "vault-tec", "vaulttec", "nuka-cola", "nukacola",
    "deathclaw", "stimpak", "stimpack", "pip-boy", "pip boy", "holotape",
    "war never changes",
]

def bin_strings(binf):
    return subprocess.run([STRINGS, "-n", "4", binf], capture_output=True,
                          text=True, errors="replace").stdout

def find_bin(d):
    if not os.path.isdir(d):
        return None
    for f in os.listdir(d):
        if f.endswith("Clip-Boy.ino.bin"):
            return os.path.join(d, f)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()

    targets = []
    if args.bin:
        targets.append((os.path.basename(os.path.dirname(args.bin)) or "bin", args.bin))
    # Default to the 4 release dirs ONLY when neither --bin nor explicit dirs given.
    default_dirs = [] if args.bin else [os.path.join(REPO, "build", x)
                    for x in ("sn34k", "res34rch", "sn34k-rift", "res34rch-rift")]
    for d in (args.dirs or default_dirs):
        b = find_bin(d)
        if b:
            targets.append((os.path.basename(d), b))

    if not targets:
        print("check_ip: no Clip-Boy.ino.bin found to scan", file=sys.stderr)
        return 2

    fails = []
    for label, b in targets:
        s = bin_strings(b).lower()
        hits = [m for m in DENYLIST if m in s]
        if hits:
            fails += [f"{label}: {h!r}" for h in hits]
            print(f"  [FAIL] {label}: " + ", ".join(repr(h) for h in hits))
        else:
            print(f"  [OK]   {label}: clean")
    print(f"\n({len(DENYLIST)} marks x {len(targets)} bin(s) checked)")

    if fails:
        print("\nIP GATE FAILED - third-party named marks in shipped strings:")
        for f in fails:
            print("  - " + f)
        print("Scrub per CLAUDE.md 'IP/parody posture' (keep references, scrub named marks).")
        return 1
    print("IP gate: PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())
