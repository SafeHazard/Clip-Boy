#!/usr/bin/env python3
"""Generate BLANK (solid-black) 320x240 RGB565 rift boot images + the presence
marker, for the PUBLIC source edition (stage_source.sh, no --ks).

The real Quantum-Rift boot art is KS-backer-exclusive (gitignored, copied only into
the Clip-Boy-KS tree). The public tree instead ships two blank boot screens so a
public `--rift` build shows plain black scenes (not the "CUSTOM BOOT SCREEN"
placeholder text, and not our real art). Output matches scripts/build_rift_screens.py
byte-format exactly (little-endian RGB565 lv_image_dsc_t, PROGMEM, #ifdef gated).

Usage: py -3 gen_blank_rift.py <OUTDIR>   # writes into <OUTDIR>/
"""
import os, sys

W, H = 320, 240

def emit(symbol, out_path):
    n = W * H * 2  # bytes; all 0x00 = black
    lines = [
        f"// Auto-generated BLANK {W}x{H} RGB565 rift boot screen (PUBLIC edition).",
        "// The real Quantum-Rift art is KS-exclusive; public --rift shows black.",
        "#ifdef BADGE_QUANTUM_RIFT",
        "#include <pgmspace.h>",
        '#include "lvgl.h"',
        "",
        f"static const uint8_t {symbol}_map[] PROGMEM = {{",
    ]
    row = "    " + ",".join(["0x00"] * 16) + ","
    lines += [row] * (n // 16)
    rem = n % 16
    if rem:
        lines.append("    " + ",".join(["0x00"] * rem) + ",")
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
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"wrote {out_path}  ({W}x{H} blank RGB565, {n} bytes)")

def main():
    if len(sys.argv) < 2:
        print("usage: gen_blank_rift.py <OUTDIR>", file=sys.stderr); sys.exit(1)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    emit("img_rift_loading", os.path.join(out, "rift_loading_img.c"))
    emit("img_rift_clippy",  os.path.join(out, "rift_clippy_img.c"))
    # presence marker so ui_nav.h uses the (blank) images instead of the
    # "CUSTOM BOOT SCREEN" placeholder bitmap.
    with open(os.path.join(out, "rift_art_present.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n// Blank public rift boot art present (see gen_blank_rift.py).\n")
    print("wrote rift_art_present.h (marker)")

if __name__ == "__main__":
    main()
