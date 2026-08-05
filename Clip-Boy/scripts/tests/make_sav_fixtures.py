#!/usr/bin/env python3
"""Generate /collectibles.sav test fixtures for DC34-92 (SD backup/restore).

These are v1 (legacy) fixtures — 44 bytes, plaintext, collectibles-only:
    [0:6]   magic  "CBSAVE"
    [6]     version (1)
    [7]     reserved (0)
    [8:40]  32-byte collected-state bitfield (bit id -> byte id//8, bit id%8)
    [40:44] CRC32 (LE) over the first 40 bytes  (standard zlib/IEEE CRC-32)

coll_crc32() in firmware is the reflected 0xEDB88320 poly == zlib.crc32, so
Python's zlib.crc32 reproduces the firmware value byte-for-byte.

NOTE (Jul 2026): a live badge now WRITES v2 (54 bytes, badge-bound XOR + ARG
progress). v1 files still import via the firmware's back-compat path (collectibles
only, no ARG). The host can't forge a valid v2 for a specific badge (it doesn't
know the eFuse MAC keystream), so v2 same-badge round-trips are tested by exporting
ON the badge; a host-made v2 would just exercise the WRONG_BADGE reject.

Usage:
    py -3 scripts/tests/make_sav_fixtures.py [out_dir]

Writes a set of fixtures into out_dir (default: scripts/tests/sav_fixtures/).
Copy any one of them onto the SD card root, RENAMED to 'collectibles.sav',
then run DATA > Settings > Restore Progress from SD.
"""
import os
import sys
import zlib

MAGIC = b"CBSAVE"
VERSION = 1
SIZE = 44

# The seeded set the ticket used for the round-trip check.
GOOD_IDS = [1, 7, 13, 21, 42, 75]
# Blacklist + out-of-range ids that must be IGNORED by the catalog on import.
PHANTOM_IDS = [0, 11, 59, 101, 200, 255]


def pack_bits(ids):
    bits = bytearray(32)
    for i in ids:
        bits[i // 8] |= 1 << (i % 8)
    return bytes(bits)


def build(ids, version=VERSION, magic=MAGIC, reserved=0,
          break_crc=False, size=SIZE):
    body = bytearray()
    body += magic
    body.append(version & 0xFF)
    body.append(reserved & 0xFF)
    body += pack_bits(ids)
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    if break_crc:
        crc ^= 0xFFFFFFFF  # guaranteed-wrong checksum
    body += crc.to_bytes(4, "little")
    # size overrides for truncated / over-long fixtures
    if size < len(body):
        body = body[:size]
    elif size > len(body):
        body += b"\x00" * (size - len(body))
    return bytes(body)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "sav_fixtures")
    os.makedirs(out, exist_ok=True)

    fixtures = {
        # name                       expected import result
        "good.sav":        build(GOOD_IDS),                       # OK: 6 finds
        "allunlocked.sav": build(range(1, 101)),                  # OK: shareable cheat (by design)
        "empty.sav":       build([]),                             # OK: 0 finds
        "phantom.sav":     build(GOOD_IDS + PHANTOM_IDS),         # OK: phantoms ignored, still 6
        "badcrc.sav":      build(GOOD_IDS, break_crc=True),       # BAD_CRC
        "badmagic.sav":    build(GOOD_IDS, magic=b"XXXXXX"),      # BAD_MAGIC
        "badversion.sav":  build(GOOD_IDS, version=2),            # BAD_VERSION
        "truncated.sav":   build(GOOD_IDS, size=40),              # BAD_SIZE (too short)
        "toolong.sav":     build(GOOD_IDS, size=48),              # BAD_SIZE (too long)
    }

    for name, data in fixtures.items():
        path = os.path.join(out, name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"  {name:16s} {len(data):3d} bytes")

    print(f"\nWrote {len(fixtures)} fixtures to {out}")
    print("Copy ONE onto SD root as 'collectibles.sav', then Import.")


if __name__ == "__main__":
    main()
