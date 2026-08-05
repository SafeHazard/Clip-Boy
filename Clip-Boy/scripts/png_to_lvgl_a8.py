"""Convert a grayscale PNG to an LVGL 9.x A8 (alpha-only) C array.

Black pixels become transparent, bright pixels become opaque.
Use image_recolor at runtime to set the display color.

Usage:
  py -3 scripts/png_to_lvgl_a8.py <input.png> <output.c> [symbol_name]

  symbol_name defaults to the output filename without extension.

Example:
  py -3 scripts/png_to_lvgl_a8.py mascot.png ClipBoyGS153x192.c ClipBoyGS153x192

Output: a .c file with lv_image_dsc_t struct, include via:
  extern "C" { LV_IMAGE_DECLARE(ClipBoyGS153x192); }

Requires: pip install Pillow
"""

import sys
from PIL import Image

def convert(png_path, out_path, var_name):
    img = Image.open(png_path).convert("L")  # 8-bit grayscale
    w, h = img.size
    pixels = img.tobytes()  # brightness values become alpha values
    stride = w  # A8 stride = width (1 byte per pixel)

    with open(out_path, "w") as f:
        f.write('#include <lvgl.h>\n\n')
        f.write(f'#ifndef LV_ATTRIBUTE_{var_name.upper()}\n')
        f.write(f'#define LV_ATTRIBUTE_{var_name.upper()}\n')
        f.write('#endif\n\n')
        f.write('static const\n')
        f.write(f'LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_{var_name.upper()}\n')
        f.write(f'uint8_t {var_name}_map[] = {{\n')

        for i, b in enumerate(pixels):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'0x{b:02x},')
            if i % 16 == 15 or i == len(pixels) - 1:
                f.write('\n')

        f.write('};\n\n')
        f.write(f'const lv_image_dsc_t {var_name} = {{\n')
        f.write('  .header = {\n')
        f.write('    .magic = LV_IMAGE_HEADER_MAGIC,\n')
        f.write('    .cf = LV_COLOR_FORMAT_A8,\n')
        f.write('    .flags = 0,\n')
        f.write(f'    .w = {w},\n')
        f.write(f'    .h = {h},\n')
        f.write(f'    .stride = {stride},\n')
        f.write('    .reserved_2 = 0,\n')
        f.write('  },\n')
        f.write(f'  .data_size = sizeof({var_name}_map),\n')
        f.write(f'  .data = {var_name}_map,\n')
        f.write('  .reserved = NULL,\n')
        f.write('};\n')

    print(f"OK: {w}x{h} A8, {len(pixels)} bytes -> {out_path}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} input.png output.c [var_name]")
        sys.exit(1)
    name = sys.argv[3] if len(sys.argv) > 3 else sys.argv[2].split("/")[-1].split("\\")[-1].replace(".c", "")
    convert(sys.argv[1], sys.argv[2], name)
