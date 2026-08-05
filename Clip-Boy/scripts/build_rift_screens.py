#!/usr/bin/env python3
"""
build_rift_screens.py -- convert the pre-composited rift boot PNGs to full-screen
RGB565 LVGL images for the BADGE_QUANTUM_RIFT build.

  images/rift/rift_loading.png            -> img_rift_loading  (opening beat)
  images/rift/rift_loading_with_clippy.png-> img_rift_clippy    (Clippy beat)

Both are 320x240, drawn opaque straight from PROGMEM (no PSRAM copy). Output is
little-endian RGB565 (LVGL native; the Waveshare panel takes it via a direct
rgb565_t cast). Both .c files are #ifdef BADGE_QUANTUM_RIFT so they cost nothing
in the normal build.

Usage:  py -3 scripts/build_rift_screens.py
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "images", "rift")
W, H = 320, 240


def emit(png_name, symbol, out_name):
    im = Image.open(os.path.join(SRC, png_name)).convert("RGB")
    if im.size != (W, H):
        im = im.resize((W, H), Image.LANCZOS)
    data = bytearray()
    for (r, g, b) in im.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data += bytes((v & 0xFF, (v >> 8) & 0xFF))   # little-endian
    lines = [
        f"// Auto-generated: {W}x{H} RGB565 rift boot screen, PROGMEM.",
        f"// Source: images/rift/{png_name}. Regenerate: py -3 scripts/build_rift_screens.py",
        "#ifdef BADGE_QUANTUM_RIFT",
        '#include <pgmspace.h>',
        '#include "lvgl.h"',
        "",
        f"static const uint8_t {symbol}_map[] PROGMEM = {{",
    ]
    for i in range(0, len(data), 16):
        lines.append("    " + ",".join(f"0x{b:02x}" for b in data[i:i+16]) + ",")
    lines += [
        "};",
        "",
        f"const lv_image_dsc_t {symbol} = {{",
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_RGB565,",
        "        .flags = 0,",
        f"        .w = {W},",
        f"        .h = {H},",
        f"        .stride = {W * 2},",
        "    },",
        f"    .data_size = sizeof({symbol}_map),",
        f"    .data = {symbol}_map,",
        "};",
        "#endif // BADGE_QUANTUM_RIFT",
        "",
    ]
    open(os.path.join(ROOT, out_name), "w", encoding="utf-8").write("\n".join(lines))
    print(f"wrote {out_name}  ({W}x{H} RGB565, {len(data)} bytes)")


emit("rift_loading.png", "img_rift_loading", "rift_loading_img.c")
emit("rift_loading_with_clippy.png", "img_rift_clippy", "rift_clippy_img.c")

# Presence marker -- ui_nav.h __has_include()s this to decide whether to draw
# the user art or fall back to the generic placeholder boot screen. Git-ignored
# along with the *_img.c, so a clean public checkout has neither.
marker = os.path.join(ROOT, "rift_art_present.h")
open(marker, "w", encoding="utf-8").write(
    "#pragma once\n"
    "// Auto-generated marker: rift boot art is present in this checkout.\n"
    "// Created by scripts/build_rift_screens.py. Git-ignored.\n")
print("wrote rift_art_present.h (marker)")
