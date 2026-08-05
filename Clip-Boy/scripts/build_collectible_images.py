#!/usr/bin/env python3
"""
build_collectible_images.py -- regenerate coll_images.c at high resolution.

Single source of truth: images/hires_src/<id>.png -- the user-curated folder
of best-available source art per collectible (hand-improved hi-res versions
live here; ID 75 is the SheetmetalCon ticket). RGBA/transparent sources are
flattened onto black (the A8 alpha mask comes from luminance, black = clear).

Reuses batch_postprocess's process_image (grayscale -> contrast -> resize ->
contrast -> raw A8) at HIRES, and generate_merged_c. Does NOT touch
coll_images.h (hand-maintained PSRAM loader); its size constants are edited
separately.

Staging goes to a TEMP dir -- it never writes into hires_src, so the curated
sources are safe. Side-by-side current-vs-new previews are written to
images/review/ for every ID whose A8 actually changed.

Usage:  py -3 scripts/build_collectible_images.py
"""

import os
import re
import sys
import glob
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)
import batch_postprocess as bp  # noqa: E402

HIRES = (200, 200)
bp.TARGET_SIZE = HIRES  # process_image / generate_merged_c read this

SRC_DIR    = os.path.join(ROOT, "images", "hires_src")    # authoritative source
STAGE_DIR  = os.path.join(ROOT, "images", "_stage_tmp")   # temp; NEVER the source
OUT_DIR    = os.path.join(ROOT, "images", "processed")
REVIEW_DIR = os.path.join(ROOT, "images", "review")
C_PATH     = os.path.join(ROOT, "coll_images.c")


def load_rgb_on_black(path):
    """Open an image and composite any transparency onto black -> RGB.
    The badge's A8 mask is luminance, and black reads as fully transparent, so
    flattening on black keeps hand-made RGBA art looking right after convert('L')."""
    im = Image.open(path)
    if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
        bg.alpha_composite(im)
        return bg.convert("RGB")
    return im.convert("RGB")


def current_bytes_map():
    """Extract the currently-shipping A8 bytes per ID from coll_images.c."""
    c = open(C_PATH, encoding="utf-8", errors="ignore").read()
    out = {}
    for nid, body in re.findall(
            r"img_coll_(\d+)_data\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", c, re.DOTALL):
        out[int(nid)] = bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+", body))
    return out


def main():
    for d in (STAGE_DIR, OUT_DIR, REVIEW_DIR):
        os.makedirs(d, exist_ok=True)

    ids = sorted(int(os.path.splitext(os.path.basename(p))[0])
                 for p in glob.glob(os.path.join(SRC_DIR, "*.png"))
                 if os.path.splitext(os.path.basename(p))[0].isdigit())
    print(f"{len(ids)} collectible source images in {os.path.relpath(SRC_DIR, ROOT)}")

    current = current_bytes_map()   # shipping art, for change detection + review
    image_data = []
    changed = []

    for i in ids:
        src = os.path.join(SRC_DIR, f"{i}.png")
        staged = os.path.join(STAGE_DIR, f"{i}.png")
        load_rgb_on_black(src).save(staged)
        _, raw = bp.process_image(staged, OUT_DIR)   # -> <id>.a8 + <id>_preview.png
        image_data.append((i, raw))
        if current.get(i) != raw:
            changed.append(i)
            _write_review(i, current.get(i), raw)

    image_data.sort(key=lambda x: x[0])
    bp.generate_merged_c(image_data, C_PATH)
    print(f"\nRegenerated {os.path.relpath(C_PATH, ROOT)} at {HIRES[0]}x{HIRES[1]}  "
          f"(~{len(image_data) * HIRES[0] * HIRES[1] // 1024} KB PROGMEM)")
    print(f"{len(changed)} image(s) changed vs current: "
          f"{', '.join(map(str, changed)) if changed else '(none)'}")
    print(f"Review side-by-sides in images/review/<id>.png")


def _write_review(i, cur_bytes, new_bytes):
    """Side-by-side: current (left) vs new (right), both shown at 200px."""
    W = HIRES[0]
    panel = Image.new("L", (W * 2 + 8, W), 40)
    if cur_bytes:
        d = int(len(cur_bytes) ** 0.5)
        if d * d == len(cur_bytes):
            cur = Image.frombytes("L", (d, d), cur_bytes)
            panel.paste(cur.resize((W, W), Image.NEAREST if d < W else Image.LANCZOS), (0, 0))
    panel.paste(Image.frombytes("L", HIRES, new_bytes), (W + 8, 0))
    panel.save(os.path.join(REVIEW_DIR, f"{i}.png"))


if __name__ == "__main__":
    main()
