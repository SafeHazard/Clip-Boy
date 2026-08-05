#!/usr/bin/env python3
"""
build_web_images.py -- assemble the public-facing collectible images.

Output: ./web_images/<id>.png, 512x512 max, RGB.

Source priority per ID:
  1. images/try2/<id>.png  (user-curated replacement; preferred when present)
  2. images/generated/<id>.png  (original procedural set)

These are what get hosted on Netlify/GitHub Pages and pointed at by
the QR codes embedded in the printed HR tags.
"""

import argparse
import csv
import os
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC_HIRES = ROOT / "images" / "hires_src"   # authoritative source (matches build_collectible_images.py)
SRC_TRY2 = ROOT / "images" / "try2"
SRC_GEN  = ROOT / "images" / "generated"
OUT_DIR  = ROOT / "web_images"
NETLIFY_REPO = ROOT.parent / "clip-boy_images"  # Netlify-deployed sibling repo

# Per the project's HR-code blacklist + 1-100 ID range.
def catalog_ids():
    """ACTUAL catalog IDs from the source-of-truth CSV, so reassignments (incl.
    IDs >100 and un-blacklisted ones) are tracked without editing a hardcoded list."""
    ids = set()
    with open(ROOT / "data" / "collectibles.csv", newline="", encoding="utf-8") as f:
        r = csv.reader(f); h = next(r); ic = h.index("ID")
        for row in r:
            try: ids.add(int(row[ic]))
            except (ValueError, IndexError): pass
    return sorted(ids)


def pick_source(id8: int) -> Path | None:
    for d in (SRC_HIRES, SRC_TRY2, SRC_GEN):
        p = d / f"{id8}.png"
        if p.exists():
            return p
    return None


def load_rgb_on_black(src: Path) -> Image.Image:
    """Load and flatten to RGB, compositing any transparency onto black (the
    hi-res sources are RGBA)."""
    im = Image.open(src)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGB", im.size, (0, 0, 0))
        bg.paste(im, mask=im.split()[-1])
        return bg
    return im.convert("RGB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--max-size", type=int, default=512,
                    help="Uniform output size in px (default 512, the deployed size). "
                         "Source is downscaled to fit if larger; it is NEVER upscaled "
                         "(re-source small originals in images/hires_src for a bigger "
                         "uniform size -- see images/hires_orig for the retired low-res set).")
    ap.add_argument("--square", action="store_true", default=True,
                    help="Pad the fitted image to a square max-size x max-size canvas on "
                         "black so every collectible is the SAME dimensions (default on).")
    ap.add_argument("--no-square", dest="square", action="store_false",
                    help="Disable square padding; keep each image's native aspect (fit-in-box).")
    ap.add_argument("--ids", type=str, default="",
                    help="Comma-separated IDs to (re)build; empty = all 1-100. "
                         "Use to update just the changed collectibles surgically.")
    ap.add_argument("--out", type=Path, default=OUT_DIR,
                    help=f"Output dir (default {OUT_DIR.relative_to(ROOT)})")
    ap.add_argument("--netlify-repo", type=Path, default=NETLIFY_REPO,
                    help="Sibling repo deployed to Netlify; receives a "
                         "mirror at <repo>/img/ + a _redirects file. "
                         "Pass empty string to skip.")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    netlify_img_dir = None
    if args.netlify_repo and str(args.netlify_repo) and args.netlify_repo.exists():
        netlify_img_dir = args.netlify_repo / "img"
        netlify_img_dir.mkdir(parents=True, exist_ok=True)

    id_filter = {int(x) for x in args.ids.split(",") if x.strip()} if args.ids.strip() else None

    summary = {"copied": [], "skipped_blacklist": [], "missing": []}
    for id8 in catalog_ids():
        if id_filter is not None and id8 not in id_filter:
            continue
        src = pick_source(id8)
        if src is None:
            summary["missing"].append(id8)
            continue
        im = load_rgb_on_black(src)
        if im.width > args.max_size or im.height > args.max_size:
            im.thumbnail((args.max_size, args.max_size), Image.LANCZOS)  # downscale only
        if args.square and im.size != (args.max_size, args.max_size):
            canvas = Image.new("RGB", (args.max_size, args.max_size), (0, 0, 0))
            canvas.paste(im, ((args.max_size - im.width) // 2,
                              (args.max_size - im.height) // 2))
            im = canvas
        # Filename = raw ID (no padding) so /coll/:id rewrites to /img/:id.png
        # without the user having to zero-pad in scanned URLs.
        out_path = args.out / f"{id8}.png"
        im.save(out_path, optimize=True)
        if netlify_img_dir is not None:
            im.save(netlify_img_dir / f"{id8}.png", optimize=True)
        summary["copied"].append((id8, str(src.relative_to(ROOT))))

    try:
        where = args.out.relative_to(ROOT)
    except ValueError:
        where = args.out
    print(f"Wrote {len(summary['copied'])} images ({args.max_size}x{args.max_size}"
          f"{', square' if args.square else ''}) to {where}")
    if netlify_img_dir is not None:
        # Drop a _redirects so QRs can encode short stable paths like /coll/48
        # and survive any future re-organization of the image tree.
        redirects = args.netlify_repo / "_redirects"
        redirects.write_text(
            "# Stable short paths used by HR-tag QR codes. Do not change\n"
            "# the LHS without re-printing every tag.\n"
            "/coll/:id    /img/:id.png    200\n"
            "/c/:id       /img/:id.png    200\n",
            encoding="utf-8",
        )
        print(f"Mirrored {len(summary['copied'])} images to "
              f"{netlify_img_dir.relative_to(args.netlify_repo)} "
              f"+ wrote {redirects.relative_to(args.netlify_repo)}")
    if summary["missing"]:
        print(f"Missing source for IDs: {summary['missing']}")
    if summary["skipped_blacklist"]:
        print(f"Skipped {len(summary['skipped_blacklist'])} blacklisted IDs")
    return 0 if not summary["missing"] else 1


if __name__ == "__main__":
    sys.exit(main())
