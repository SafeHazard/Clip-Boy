"""Post-process generated PNGs into 80x80 A8 files for Clip-Boy badge.

Takes the 512x512 PNGs from comfyui_batch.py and:
1. Converts to grayscale
2. Applies contrast enhancement (crush blacks, boost whites)
3. Downscales to 80x80 with LANCZOS
4. Exports as raw A8 (alpha-only bytes) for SD card / LittleFS
5. Generates merged coll_images.c + coll_images.h for firmware embedding

Usage:
  py -3 scripts/batch_postprocess.py --input images/generated/ --output images/processed/
  py -3 scripts/batch_postprocess.py --input images/generated/ --output images/processed/ --ids 2,3,40
  py -3 scripts/batch_postprocess.py --input images/generated/42.png --output images/processed/

Outputs:
  images/processed/<id>.a8          — raw A8 for SD card
  images/processed/<id>_preview.png — preview for visual review
  coll_images.c                     — merged PROGMEM data (all images)
  coll_images.h                     — declarations + lookup function

Requires: pip install Pillow
"""

import argparse
import os
import re
import sys
import glob

try:
    from PIL import Image, ImageOps
except ImportError:
    print("ERROR: Pillow required. Install: pip install Pillow", file=sys.stderr)
    sys.exit(1)

TARGET_SIZE = (200, 200)
BLACK_CRUSH = 20     # Pixels below this brightness become 0 (transparent)
WHITE_BOOST = 200    # Pixels above this brightness become 255 (fully opaque)


def enhance_contrast(img, black_crush=BLACK_CRUSH, white_boost=WHITE_BOOST):
    """Crush near-blacks and boost near-whites for better alpha mask contrast."""
    pixels = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            v = pixels[x, y]
            if v < black_crush:
                pixels[x, y] = 0
            elif v > white_boost:
                pixels[x, y] = 255
            else:
                pixels[x, y] = int((v - black_crush) * 255 / (white_boost - black_crush))
    return img


def process_image(input_path, output_dir):
    """Process a single PNG into an 80x80 A8 file. Returns (id, raw_bytes) or None."""
    basename = os.path.splitext(os.path.basename(input_path))[0]

    img = Image.open(input_path).convert("L")  # Grayscale
    img = enhance_contrast(img)
    img = img.resize(TARGET_SIZE, Image.LANCZOS)
    img = enhance_contrast(img, black_crush=10, white_boost=245)

    # Save raw A8
    a8_path = os.path.join(output_dir, f"{basename}.a8")
    raw = img.tobytes()
    with open(a8_path, "wb") as f:
        f.write(raw)

    # Save preview PNG
    preview_path = os.path.join(output_dir, f"{basename}_preview.png")
    img.save(preview_path)

    print(f"  {basename}: {TARGET_SIZE[0]}x{TARGET_SIZE[1]}, {len(raw)} bytes -> {a8_path}")
    return basename, raw


def generate_merged_c(image_data, output_path):
    """Generate a single merged .c file with all image data."""
    w, h = TARGET_SIZE

    lines = []
    lines.append(f"// Auto-generated: collectible images, {w}x{h} A8, PROGMEM")
    lines.append("// Regenerate: py -3 scripts/batch_postprocess.py")
    lines.append("//")
    lines.append("// Do not edit manually.")
    lines.append("")
    lines.append('#include <pgmspace.h>')
    lines.append('#include "lvgl.h"')
    lines.append("")

    for img_id, raw in image_data:
        symbol = f"img_coll_{img_id}"
        hex_lines = []
        for i in range(0, len(raw), 16):
            chunk = raw[i:i+16]
            hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
            hex_lines.append(f"    {hex_str},")

        lines.append(f"// -- ID {img_id} ({len(raw)} bytes) --")
        lines.append(f"static const uint8_t {symbol}_data[] PROGMEM = {{")
        lines.extend(hex_lines)
        lines.append("};")
        lines.append("")
        lines.append(f"const lv_image_dsc_t {symbol} = {{")
        lines.append("    .header = {")
        lines.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
        lines.append("        .cf = LV_COLOR_FORMAT_A8,")
        lines.append(f"        .w = {w},")
        lines.append(f"        .h = {h},")
        lines.append("    },")
        lines.append(f"    .data_size = {len(raw)},")
        lines.append(f"    .data = {symbol}_data,")
        lines.append("};")
        lines.append("")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"  -> {output_path} ({len(image_data)} images, {os.path.getsize(output_path) // 1024}KB)")


def generate_merged_h(image_ids, output_path):
    """Generate header with declarations and lookup function."""
    lines = []
    lines.append("#pragma once")
    lines.append(f"// Auto-generated collectible image declarations + lookup table")
    lines.append(f"// {len(image_ids)} images, 80x80 A8, ~{len(image_ids) * 6400 // 1024}KB PROGMEM total")
    lines.append("// Regenerate: py -3 scripts/batch_postprocess.py")
    lines.append("")
    lines.append('extern "C" {')
    for i in image_ids:
        lines.append(f"    LV_IMAGE_DECLARE(img_coll_{i});")
    lines.append("}")
    lines.append("")
    lines.append("// Lookup: collectible ID -> compiled-in image (or NULL)")
    lines.append("static const lv_image_dsc_t* coll_get_builtin_image(uint8_t id) {")
    lines.append("    switch (id) {")
    for i in image_ids:
        lines.append(f"        case {i}: return &img_coll_{i};")
    lines.append("        default: return NULL;")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"  -> {output_path} ({len(image_ids)} entries)")


def main():
    parser = argparse.ArgumentParser(description="Post-process generated images to 80x80 A8")
    parser.add_argument("--input", required=True, help="Input directory (or single PNG file)")
    parser.add_argument("--output", default="processed", help="Output directory for .a8 + previews")
    parser.add_argument("--sketch", default=".", help="Sketch directory for coll_images.c/.h")
    parser.add_argument("--ids", default=None, help="Comma-separated IDs to process (default: all)")
    parser.add_argument("--black-crush", type=int, default=20,
                        help="Pixels below this become transparent (default: 20)")
    parser.add_argument("--white-boost", type=int, default=200,
                        help="Pixels above this become opaque (default: 200)")
    args = parser.parse_args()

    BLACK_CRUSH = args.black_crush
    WHITE_BOOST = args.white_boost

    os.makedirs(args.output, exist_ok=True)

    # Collect input files
    if os.path.isfile(args.input):
        files = [args.input]
    else:
        files = sorted(glob.glob(os.path.join(args.input, "*.png")))

    if not files:
        print(f"ERROR: No PNG files found in {args.input}", file=sys.stderr)
        sys.exit(1)

    # Filter by IDs
    if args.ids:
        id_set = set(args.ids.split(","))
        files = [f for f in files if os.path.splitext(os.path.basename(f))[0] in id_set]

    print(f"Processing {len(files)} images -> {args.output}/")

    # Process all images, collect raw data for merged output
    image_data = []  # list of (id_int, raw_bytes)
    for f in files:
        try:
            basename, raw = process_image(f, args.output)
            try:
                img_id = int(basename)
                image_data.append((img_id, raw))
            except ValueError:
                pass  # Non-numeric filename, skip from merged output
        except Exception as e:
            print(f"  FAIL {f}: {e}", file=sys.stderr)

    # Sort by ID for clean output
    image_data.sort(key=lambda x: x[0])
    image_ids = [x[0] for x in image_data]

    print(f"\nDone: {len(image_data)}/{len(files)} processed")
    print(f"A8 files ready for SD card at /images/<id>.a8")

    # Generate merged coll_images.c (data arrays only).
    # NOTE: we deliberately do NOT regenerate coll_images.h here. The shipping
    # coll_images.h is hand-maintained -- it holds the PSRAM bulk-load cache,
    # the id->index lookup map and the COLL_IMG_* size constants. The old
    # generate_merged_h() emits a naive switch-only header that would clobber
    # all of that. To rebuild the full image set with correct source priority
    # (try2 > generated, ID 75 = ticket) use:
    #     py -3 scripts/build_collectible_images.py
    if image_data:
        c_path = os.path.join(args.sketch, "coll_images.c")
        print(f"\nGenerating merged firmware file:")
        generate_merged_c(image_data, c_path)
        print("  (coll_images.h left untouched -- it is hand-maintained)")


if __name__ == "__main__":
    main()
