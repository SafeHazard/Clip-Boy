#!/usr/bin/env python3
"""
build_qr_codes.py -- generate one "print sheet" PNG per collectible.

The sheet is a TALLER-than-square 2-color mask fed to hm_codegen.py via --qr:

  ┌──────────────────────┐
  │   Scan me with your   │   HEADER band (flat, no bumps) -- monofonto, upright
  ├───────────────────────┤ ← top of the 60mm HR square
  │       Clip-Boy        │   Clip-Boy -- Trek font (TOS_Title.ttf), upright, LARGER
  │   ▛▀▖  ▗▄▖   ▝▀▚      │
  │  ▐   QR (>=44.5mm)  ▌  │   QR -- centered on the square, below Clip-Boy
  │   ▙▄▟   ▄▄▘           │
  └───────────────────────┘

The bottom `square_mm` (60) of the sheet maps onto the HR-code square (bumps);
the top `header_mm` maps onto a flat header band that hm_codegen extends the
trapezoid up over. hm_codegen resizes preserving aspect and splits at the header
fraction, so both scripts must agree on --header-mm (build_all_tags passes it).

Usage:
  py -3 scripts/build_qr_codes.py --base clipboy.netlify.app --header-mm 15
"""

import argparse
import csv
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

import qrcode
from qrcode.constants import ERROR_CORRECT_M

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "qr_codes"

# ── Geometry (mm), rasterised at PX_PER_MM ──────────────────────────────────
SQUARE_MM     = 60.0     # HR-code square (fixed; matches hm_codegen --size)
QR_BLACK_MM   = 43.5     # QR BLACK-module extent (owner target 435px @ 10px/mm)
PX_PER_MM     = 10       # source raster density (hm_codegen downsamples)
MARGIN_MM     = 2.0      # side margin for text so strokes don't touch the edge
QR_BOTTOM_MM  = 0.5      # QR ends this far off the square bottom (fills bottom row)
CLIPBOY_H_MM  = 14.1     # Clip-Boy ink height target (owner hand-tuned: 141px)
CLIPBOY_TOP_MM= 0.5      # Clip-Boy ink-top, below the header divider (owner: y=85)
STROKE_MIN_MM = 1.0      # legibility floor: thinnest stroke at print scale

HEADER_TEXT  = "Scan me with your"
CLIPBOY_TEXT = "Clip-Boy"


def catalog_ids():
    """Actual catalog IDs from the source-of-truth CSV (tracks reassignments)."""
    ids = set()
    with open(ROOT / "data" / "collectibles.csv", newline="", encoding="utf-8") as f:
        r = csv.reader(f); h = next(r); ic = h.index("ID")
        for row in r:
            try: ids.add(int(row[ic]))
            except (ValueError, IndexError): pass
    return sorted(ids)


# ── Fonts (strict: missing required font = hard error) ──────────────────────
def _monofonto_paths() -> list[str]:
    import os
    out = []
    # Honour the same env var build_fonts.sh uses, first.
    env = os.environ.get("MONOFONTO_OTF")
    if env:
        out.append(env)
    # esp/documents is build_fonts.sh's default home for the (uncommitted, EULA)
    # monofonto OTF; it's a .otf, not .ttf.
    out.append(str(ROOT.parent / "documents" / "monofonto rg.otf"))
    for base in [
        str(ROOT.parent / "documents"),
        str(ROOT), str(ROOT / "fonts"), str(ROOT / "assets" / "fonts"),
        str(ROOT / "scripts"),
        "C:/Users/data/OneDrive/Documents", "C:/Users/data/Documents",
        "C:/Windows/Fonts",
    ]:
        for name in ["monofonto rg.otf", "monofonto.otf", "Monofonto.otf",
                     "monofonto.ttf", "Monofonto.ttf", "MONOFONTO.TTF",
                     "monofonto rg.ttf", "Monofonto Rg.ttf"]:
            out.append(f"{base}/{name}")
    return out


def _trek_paths() -> list[str]:
    # "Trek" == TOS_Title.ttf (family "Trek"), the badge's Clip-Boy wordmark font,
    # installed as a per-user font.
    import os
    local = os.environ.get("LOCALAPPDATA", "C:/Users/data/AppData/Local")
    out = []
    for base in [f"{local}/Microsoft/Windows/Fonts", "C:/Windows/Fonts",
                 "C:/Users/data/OneDrive/esp/ui_test/assets/fonts"]:
        for name in ["TOS_Title.ttf", "Trek.ttf", "trek.ttf"]:
            out.append(f"{base}/{name}")
    return out


def _load_or_die(paths: list[str], size: int, label: str):
    for p in paths:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    sys.exit(f"[build_qr_codes] REQUIRED font not found: {label}. "
             f"Looked in:\n  " + "\n  ".join(paths))


def load_monofonto(size: int):
    return _load_or_die(_monofonto_paths(), size, "monofonto (header text)")


def load_trek(size: int):
    return _load_or_die(_trek_paths(), size, "Trek / TOS_Title.ttf (Clip-Boy)")


def fit_font(loader, text: str, max_w: int, max_h: int, cap: int = 600):
    """Largest font size whose `text` fits within (max_w, max_h). Returns (font, size)."""
    probe = ImageDraw.Draw(Image.new("L", (8, 8)))
    lo, hi, best = 8, cap, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        f = loader(mid)
        b = probe.textbbox((0, 0), text, font=f)
        if (b[2] - b[0]) <= max_w and (b[3] - b[1]) <= max_h:
            best = mid; lo = mid + 1
        else:
            hi = mid - 1
    return loader(best), best


def draw_centered(canvas: Image.Image, text: str, font, cx: int, cy: int):
    """Draw `text` black, centered on (cx, cy) using the true ink bbox."""
    d = ImageDraw.Draw(canvas)
    b = d.textbbox((0, 0), text, font=font)          # ink bbox at origin
    w, h = b[2] - b[0], b[3] - b[1]
    d.text((cx - w / 2 - b[0], cy - h / 2 - b[1]), text, fill=0, font=font)


def draw_qr(url: str, black_px: int, border_modules: int = 0) -> Image.Image:
    """QR whose BLACK-module extent is exactly `black_px`. border_modules adds a
    white quiet ring in-image; 0 relies on the surrounding white plate for the
    quiet zone (the QR sits centered on the white square with wide side margins)."""
    qr = qrcode.QRCode(version=None, error_correction=ERROR_CORRECT_M,
                       box_size=10, border=0)     # no in-image border; we control it
    qr.add_data(url)
    qr.make(fit=True)
    n = qr.modules_count
    core = qr.make_image(fill_color="black", back_color="white").convert("L")
    core = core.resize((black_px, black_px), Image.NEAREST)
    if border_modules <= 0:
        return core
    bp = int(round(border_modules * black_px / n))
    full = Image.new("L", (black_px + 2 * bp, black_px + 2 * bp), 255)
    full.paste(core, (bp, bp))
    return full




def render_tag(id8: int, base_url: str, header_mm: float) -> Image.Image:
    url = f"https://{base_url}/coll/{id8}"

    S        = round(SQUARE_MM * PX_PER_MM)           # square region px (600)
    Hh       = round(header_mm * PX_PER_MM)           # header band px
    margin   = round(MARGIN_MM * PX_PER_MM)
    qr_black = round(QR_BLACK_MM * PX_PER_MM)         # QR black extent px (435)
    bottom_m = round(QR_BOTTOM_MM * PX_PER_MM)

    canvas = Image.new("L", (S, Hh + S), 255)

    # --- Header band (short): "Scan me with your" (monofonto), centered ---
    hf, hsz = fit_font(load_monofonto, HEADER_TEXT,
                       max_w=S - 2 * margin, max_h=Hh - round(1.0 * PX_PER_MM))
    draw_centered(canvas, HEADER_TEXT, hf, S // 2, Hh // 2)

    # --- QR: black extent = qr_black, anchored to end ~QR_BOTTOM_MM off the
    #     square bottom (fills the bottom row); centered horizontally. ---
    sq_top    = Hh
    qr_bottom = (Hh + S) - bottom_m
    qr_top    = qr_bottom - qr_black

    # --- Clip-Boy: sized + positioned to the owner's hand-tuned placement --
    #     ink height ~CLIPBOY_H_MM, ink-top CLIPBOY_TOP_MM below the header
    #     divider, centered horizontally. Leaves a comfortable QR quiet-gap. ---
    cf, csz = fit_font(load_trek, CLIPBOY_TEXT,
                       max_w=S - 2 * margin, max_h=round(CLIPBOY_H_MM * PX_PER_MM))
    d = ImageDraw.Draw(canvas)
    fb = d.textbbox((0, 0), CLIPBOY_TEXT, font=cf)     # ink bbox incl descenders
    y0 = sq_top + round(CLIPBOY_TOP_MM * PX_PER_MM) - fb[1]   # ink-top placement
    d.text((S // 2 - (fb[2] - fb[0]) / 2 - fb[0], y0), CLIPBOY_TEXT, fill=0, font=cf)

    qr_img = draw_qr(url, qr_black, border_modules=0)
    canvas.paste(qr_img, ((S - qr_black) // 2, qr_top))
    return canvas


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default="clipboy.netlify.app",
                    help="Hostname for the QR URL: https://<base>/coll/<id>.")
    ap.add_argument("--out", type=Path, default=OUT_DIR,
                    help=f"Output dir (default {OUT_DIR.relative_to(ROOT)})")
    ap.add_argument("--header-mm", type=float, default=8.0,
                    help="Flat header band height in mm (default 8). MUST match "
                         "the --header-mm passed to hm_codegen (build_all_tags "
                         "keeps them in sync).")
    ap.add_argument("--ids", type=str, default=None,
                    help="Comma-separated subset of IDs to build (e.g. '48').")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    wanted = ([int(x) for x in args.ids.split(",")] if args.ids else catalog_ids())
    written = 0
    for id8 in wanted:
        img = render_tag(id8, args.base, args.header_mm)
        img.save(args.out / f"{id8}.png", optimize=True)
        written += 1

    if not args.ids:  # full run: prune QR PNGs for IDs no longer in the catalog
        keep = {f"{i}.png" for i in catalog_ids()}
        for p in args.out.glob("*.png"):
            if p.name not in keep:
                p.unlink(); print(f"  removed stale {p.name}")

    try:
        where = args.out.relative_to(ROOT)
    except ValueError:
        where = args.out
    print(f"Wrote {written} print-sheet PNGs to {where} "
          f"(header {args.header_mm}mm + {SQUARE_MM:.0f}mm square)")
    print(f"URL pattern: https://{args.base}/coll/<id>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
