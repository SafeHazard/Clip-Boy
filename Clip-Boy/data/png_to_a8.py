"""Convert a grayscale PNG to raw A8 (alpha-only) for Clip-Boy SD card modding.

Output is raw bytes — no header, no palette. 1 byte per pixel, top-left to
bottom-right.  Bright pixels = opaque, black pixels = transparent.

Output is square 200x200 by default (the badge infers the dimension from the
file size, so any square size works -- 80x80 is still accepted). Name the
output file <id>.a8 where <id> is the collectible's numeric ID, and place it
in /images/ on the SD card.

Requires: pip install Pillow

Usage: py -3 png_to_a8.py input.png [output.a8] [size]
       If output is omitted, replaces .png extension with .a8
       size defaults to 200 (edge length in px)
"""

import sys
from PIL import Image

def convert(png_path, out_path, size=200):
    img = Image.open(png_path).convert("L")  # 8-bit grayscale
    img = img.resize((size, size), Image.LANCZOS)
    w, h = img.size
    raw = img.tobytes()
    with open(out_path, "wb") as f:
        f.write(raw)
    print(f"OK: {w}x{h}, {len(raw)} bytes -> {out_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.png [output.a8]")
        sys.exit(1)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".a8"
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    convert(src, dst, size)
