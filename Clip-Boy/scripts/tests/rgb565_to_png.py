#!/usr/bin/env python3
"""rgb565_to_png.py <in.raw> [w h] — convert a raw little-endian RGB565 screen
dump (session-mode 'screenshot' output) to PNG for viewing."""
import sys
from PIL import Image

f = sys.argv[1]
w = int(sys.argv[2]) if len(sys.argv) > 2 else 320
h = int(sys.argv[3]) if len(sys.argv) > 3 else 240
data = open(f, "rb").read()
img = Image.new("RGB", (w, h))
px = img.load()
for y in range(h):
    for x in range(w):
        o = (y * w + x) * 2
        if o + 1 >= len(data):
            continue
        p = data[o] | (data[o + 1] << 8)
        r = ((p >> 11) & 0x1F) * 255 // 31
        g = ((p >> 5) & 0x3F) * 255 // 63
        b = (p & 0x1F) * 255 // 31
        px[x, y] = (r, g, b)
out = f.rsplit(".", 1)[0] + ".png"
img.save(out)
print(out)
