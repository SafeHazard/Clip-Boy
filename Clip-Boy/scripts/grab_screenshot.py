#!/usr/bin/env python3
"""grab_screenshot.py — one-shot, on-demand badge screenshot.

Auto-detects the ESP32-S3 serial port (it changes when you plug in a different
badge — the VID 0x303A doesn't) and writes a date/time-stamped PNG (pure-stdlib
encoder, no Pillow — hand the file straight to a reviewer). Similar to:

    py -3 scripts/test_bridge.py --port COMx screenshot --file myshot.bmp

but with no port to look up and no filename to invent. Captures whatever is
currently on the badge's screen.

Requires a --test (TEST_HARNESS) build — the `screenshot` command lives in the
test harness. Navigate the badge by touch first, then run this.

Usage:
    py -3 scripts/grab_screenshot.py                    # auto-port, cwd
    py -3 scripts/grab_screenshot.py --port COM8        # force a port
    py -3 scripts/grab_screenshot.py --out-dir shots    # choose a folder
    py -3 scripts/grab_screenshot.py --name menu        # filename prefix
"""
import os
import sys
import zlib
import struct
import argparse
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_bridge import find_esp32_port, Bridge


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def rgb565_to_png(data: bytes, width: int, height: int, stride: int) -> bytes:
    """Encode a top-down RGB565 framebuffer as a PNG (pure stdlib, no Pillow)."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)                      # per-row filter type 0 (none)
        base = y * stride
        for x in range(width):
            i = base + x * 2
            px = data[i] | (data[i + 1] << 8)   # little-endian RGB565
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            raw.append((r << 3) | (r >> 2))     # 5/6/5 -> 8/8/8
            raw.append((g << 2) | (g >> 4))
            raw.append((b << 3) | (b >> 2))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit truecolor
    return (b"\x89PNG\r\n\x1a\n" +
            _png_chunk(b"IHDR", ihdr) +
            _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            _png_chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description="One-shot timestamped badge screenshot")
    ap.add_argument("--port", help="Serial port (default: auto-detect ESP32-S3 by VID)")
    ap.add_argument("--out-dir", default=".", help="Output directory (default: current dir)")
    ap.add_argument("--name", default="screenshot", help="Filename prefix (default: screenshot)")
    args = ap.parse_args()

    port = args.port or find_esp32_port()
    if not port:
        print("ERROR: no ESP32-S3 badge found. Plug it in (and flash a --test build).",
              file=sys.stderr)
        return 2
    print(f"Using port {port}")

    os.makedirs(args.out_dir, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = os.path.join(args.out_dir, f"{args.name}_{stamp}.png")

    bridge = Bridge(port, timeout=10.0)
    try:
        resp = bridge.send_command("screenshot")
        if not resp.get("ok"):
            print(f"ERROR: screenshot command failed: {resp}", file=sys.stderr)
            return 1
        size = resp.get("size", 0)
        width = resp.get("width", 320)
        height = resp.get("height", 240)
        stride = resp.get("stride", width * 2)
        data = bridge.read_binary(size)
    finally:
        bridge.close()

    png = rgb565_to_png(data, width, height, stride)
    with open(out, "wb") as f:
        f.write(png)
    print(f"Saved {out}  ({width}x{height}, {len(png)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
