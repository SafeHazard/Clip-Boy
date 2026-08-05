#!/usr/bin/env python3
"""
import_rift_assets.py -- pull the SpaceBadge loading-screen art into Clip-Boy
for the BADGE_QUANTUM_RIFT special build.

SpaceBadge is a 240x320 PORTRAIT display; Clip-Boy is 320x240 LANDSCAPE. The
starfield is decoded from SpaceBadge's RGB565 C array and rotated 90deg to fit
our screen. The shuttle is a small RGB565A8 sprite copied verbatim (just
renamed). Both outputs are wrapped in #ifdef BADGE_QUANTUM_RIFT so they only
cost flash in the rift build.

Outputs (sketch root):
  rift_stars.c    -> img_rift_stars   (320x240 RGB565)
  rift_shuttle.c  -> img_rift_shuttle (30x30 RGB565A8, unchanged pixels)

Usage:  py -3 scripts/import_rift_assets.py
"""
import os
import re

SB = (r"C:\Users\data\OneDrive\Documents\Arduino\SpaceBadge"
      r"\src\ST7701_for_ESP32_WS_Driver_Board\src\ui")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_map(path, sym):
    txt = open(path, encoding="utf-8", errors="ignore").read()
    m = re.search(re.escape(sym) + r"\[\]\s*=\s*\{(.*?)\};", txt, re.DOTALL)
    if not m:
        raise SystemExit(f"map {sym} not found in {path}")
    return bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1)))


def emit_c(out_path, symbol, cf, w, h, data, guard="BADGE_QUANTUM_RIFT"):
    # LVGL stride = the COLOR-plane row stride = w*2 for both RGB565 and
    # RGB565A8 (the A8 plane follows the color block; data_size carries the
    # extra alpha bytes). SpaceBadge's original shuttle used stride=60 (30*2).
    stride = w * 2
    lines = [
        f"// Auto-generated for the rift boot build: {w}x{h} {cf}.",
        "// Source: SpaceBadge EEZ-Studio export, re-imported by",
        "// scripts/import_rift_assets.py. Compiled only when BADGE_QUANTUM_RIFT.",
        "",
        f"#ifdef {guard}",
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
        f"        .cf = LV_COLOR_FORMAT_{cf},",
        "        .flags = 0,",
        f"        .w = {w},",
        f"        .h = {h},",
        f"        .stride = {stride},",
        "    },",
        f"    .data_size = sizeof({symbol}_map),",
        f"    .data = {symbol}_map,",
        "};",
        f"#endif // {guard}",
        "",
    ]
    open(out_path, "w", encoding="utf-8").write("\n".join(lines))
    print(f"Wrote {out_path}  ({w}x{h} {cf}, {len(data)} bytes)")


def main():
    from PIL import Image

    # --- stars: 240x320 RGB565 -> rotate to 320x240 ---
    raw = read_map(os.path.join(SB, "ui_image_stars.c"), "img_stars_map")
    sw, sh = 240, 320
    im = Image.new("RGB", (sw, sh))
    px = im.load()
    for y in range(sh):
        for x in range(sw):
            lo = raw[(y * sw + x) * 2]
            hi = raw[(y * sw + x) * 2 + 1]
            val = lo | (hi << 8)
            px[x, y] = (((val >> 11) & 0x1F) << 3,
                        ((val >> 5) & 0x3F) << 2,
                        (val & 0x1F) << 3)
    im = im.rotate(90, expand=True)            # 240x320 -> 320x240
    im = im.resize((320, 240), Image.LANCZOS)  # guarantee exact size
    out = bytearray()
    p = im.load()
    for y in range(240):
        for x in range(320):
            r, g, b = p[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out += bytes((v & 0xFF, (v >> 8) & 0xFF))
    emit_c(os.path.join(ROOT, "rift_stars.c"), "img_rift_stars", "RGB565", 320, 240, out)

    # --- shuttle: 30x30 RGB565A8 -> verbatim ---
    sh_raw = read_map(os.path.join(SB, "ui_image_ui___shuttle.c"), "img_ui___shuttle_map")
    emit_c(os.path.join(ROOT, "rift_shuttle.c"), "img_rift_shuttle", "RGB565A8", 30, 30, sh_raw)


if __name__ == "__main__":
    main()
