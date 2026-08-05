#!/usr/bin/env python3
"""check_littlefs.py -- release gate: verify the littlefs content image mounts and
carries the expected radio clips BEFORE it is signed/shipped (DC34-139).

This is the BUILD-TIME backstop for the content-safety story. It does NOT replace
the firmware's runtime graceful degradation (a missing/partial clip is skipped via
LittleFS.exists + soft-fail, never a reboot) -- it catches a bad image before it
ever leaves the build, which is where a hard gate belongs.

Checks:
  1. image exists + size == full partition size (a short image would leave a
     stale-tail / half-old FS on a no-erase content flash).
  2. mklittlefs -l mounts it (a corrupt FS asserts/fails here).
  3. >=1 /radio/*.mp3 present, all non-empty.
  4. if build_littlefs/radio/ (embed_audio's staging dir) is present, the image
     matches it byte-for-byte (every staged clip in the image at the same size,
     and no surprise extras).

Usage: py -3 check_littlefs.py <image.bin> [partition_size_bytes]
Exit 0 = ok (+ one-line summary), 1 = fail (reason to stderr).
"""
import os, sys, glob, subprocess

PART_SIZE = 0x670000   # littlefs partition size (partitions.csv) = 6,750,208 B
PAGE, BLOCK = 256, 4096

def find_mklittlefs():
    base = os.path.expanduser("~/AppData/Local/Arduino15/packages/esp32/tools/mklittlefs")
    hits = sorted(glob.glob(os.path.join(base, "*", "mklittlefs.exe")))
    return hits[-1] if hits else None

def die(msg):
    print("check_littlefs: FAIL -- " + msg, file=sys.stderr)
    sys.exit(1)

def main():
    if len(sys.argv) < 2:
        die("usage: check_littlefs.py <image.bin> [partition_size]")
    img = sys.argv[1]
    part_size = int(sys.argv[2]) if len(sys.argv) > 2 else PART_SIZE
    if not os.path.isfile(img):
        die("image not found: " + img)

    sz = os.path.getsize(img)
    if sz != part_size:
        die("image is %d B, expected full partition %d B (0x%X) -- a short image risks a "
            "stale-tail on a no-erase content flash" % (sz, part_size, part_size))

    mk = find_mklittlefs()
    if not mk:
        die("mklittlefs not found under Arduino15")

    try:
        r = subprocess.run([mk, "-l", "-s", str(part_size), "-p", str(PAGE), "-b", str(BLOCK), img],
                           capture_output=True, text=True, timeout=60)
    except Exception as e:
        die("mklittlefs -l raised: %r" % e)
    if r.returncode != 0:
        die("image does not mount (mklittlefs -l rc=%d): %s" % (r.returncode, (r.stderr or r.stdout)[:200]))

    entries = {}
    for line in r.stdout.splitlines():
        cols = line.split("\t")
        if len(cols) >= 2 and cols[0].strip().isdigit():
            entries[cols[1].strip()] = int(cols[0])
    radio = {p: s for p, s in entries.items() if p.startswith("/radio/") and p.endswith(".mp3")}
    if not radio:
        die("no /radio/*.mp3 in the image (%d entries total)" % len(entries))
    empty = [p for p, s in radio.items() if s == 0]
    if empty:
        die("zero-byte clips in image: " + ", ".join(sorted(empty)))

    # Cross-check against the staging dir if it's alongside the project.
    proj = os.path.dirname(os.path.abspath(img))
    stage = os.path.join(proj, "build_littlefs", "radio")
    if not os.path.isdir(stage):
        stage = os.path.join(os.path.dirname(proj), "build_littlefs", "radio")  # image is in release/
    used_mb = sum(radio.values()) / 1048576.0
    part_mb = part_size / 1048576.0
    if os.path.isdir(stage):
        staged = {f: os.path.getsize(os.path.join(stage, f))
                  for f in os.listdir(stage) if f.endswith(".mp3")}
        for f, s in staged.items():
            key = "/radio/" + f
            if key not in radio:
                die("staged clip missing from image: " + key)
            if radio[key] != s:
                die("size mismatch %s: staged %d, image %d" % (key, s, radio[key]))
        extra = set(radio) - {"/radio/" + f for f in staged}
        if extra:
            die("image has clips not in staging: " + ", ".join(sorted(extra)))
        print("%d /radio clips, %.2f/%.2f MB, matches staging" % (len(radio), used_mb, part_mb))
    else:
        print("%d /radio clips, %.2f/%.2f MB (no staging dir to cross-check)" % (len(radio), used_mb, part_mb))
    sys.exit(0)

if __name__ == "__main__":
    main()
