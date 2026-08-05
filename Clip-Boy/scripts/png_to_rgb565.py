#!/usr/bin/env python3
"""
png_to_rgb565.py -- convert a PNG to a full-color LVGL RGB565 image array.

Used for collectibles that get a full-color fullscreen view (the thumbnail
and 'you found' modal stay A8/mono-tinted; only the fullscreen is color).
The image is composited onto black (the fullscreen background) so any source
transparency reads correctly, resized square, and packed as little-endian
RGB565 -- LVGL's native order, which the Waveshare panel takes via a direct
rgb565_t cast (no byte swap in disp_flush_cb).

Usage:
  py -3 scripts/png_to_rgb565.py <input.png> <symbol> [size]
    <symbol>  e.g. img_coll_color_75  -> writes <symbol>.c next to the sketch
    size      square edge length in px (default 240)
"""
import os
import sys
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    symbol = sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 240

    im = Image.open(src).convert("RGBA")
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    bg.alpha_composite(im)
    rgb = bg.convert("RGB").resize((size, size), Image.LANCZOS)

    px = rgb.load()
    data = bytearray()
    for y in range(size):
        for x in range(size):
            r, g, b = px[x, y]
            val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data.append(val & 0xFF)         # little-endian: low byte first
            data.append((val >> 8) & 0xFF)

    stride = size * 2
    out_path = os.path.join(ROOT, f"{symbol}.c")
    lines = [
        f"// Auto-generated full-color image: {size}x{size} RGB565, PROGMEM",
        f"// Regenerate: py -3 scripts/png_to_rgb565.py <png> {symbol} {size}",
        "// Do not edit manually.",
        "",
        '#include <pgmspace.h>',
        '#include "lvgl.h"',
        "",
        f"static const uint8_t {symbol}_data[] PROGMEM = {{",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",")
    lines += [
        "};",
        "",
        f"const lv_image_dsc_t {symbol} = {{",
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_RGB565,",
        f"        .w = {size},",
        f"        .h = {size},",
        f"        .stride = {stride},",
        "    },",
        f"    .data_size = {len(data)},",
        f"    .data = {symbol}_data,",
        "};",
        "",
    ]
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Wrote {out_path}  ({size}x{size} RGB565, {len(data)} bytes, "
          f"{os.path.getsize(out_path)//1024}KB source)")


if __name__ == "__main__":
    main()
