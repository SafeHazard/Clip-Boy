#!/usr/bin/env python3
"""
build_all_tags.py -- generate every collectible's printable .3mf tag.

For each non-blacklisted ID 1..100, runs hm_codegen.py with:
  - the corresponding QR PNG from qr_codes/<id>.png  (built by build_qr_codes.py)
  - the moat border (5 mm wide, 4 mm deep) for reliable LiDAR FAR cluster
  - the smooth-text resampling path (LANCZOS at 256 px) so 'Scan With
    Clip-Boy' renders cleanly through the codegen pipeline

Output: tags/<id>.3mf -- one per collectible, ready to slice and print.

Usage:
  py -3 scripts/build_all_tags.py
"""

import argparse
import csv
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HM_CODEGEN = ROOT.parent / "libraries" / "HRCode4x4" / "examples" / "hm_codegen.py"
QR_DIR = ROOT / "qr_codes"
OUT_DIR = ROOT / "tags"


def catalog_ids():
    """Real catalog IDs from the source-of-truth CSV -- tracks the reliability
    reassignments (24->124, 25->127, 40->108, 41->112, 43->118, ...; 82 was a failed
    24 intermediate, not a catalog ID). MUST match build_qr_codes.py, which only emits
    QR PNGs for these IDs. The old hardcoded range(1,101)-blacklist built stale IDs
    (24/25/40/41/43 -> no QR -> failed) and skipped the remapped 108/112/118/124/127."""
    ids = set()
    with open(ROOT / "data" / "collectibles.csv", newline="", encoding="utf-8") as f:
        r = csv.reader(f); h = next(r); ic = h.index("ID")
        for row in r:
            try: ids.add(int(row[ic]))
            except (ValueError, IndexError): pass
    return sorted(ids)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qr-dir", type=Path, default=QR_DIR,
                    help="Where the per-ID QR PNGs live "
                         f"(default {QR_DIR.relative_to(ROOT)})")
    ap.add_argument("--out", type=Path, default=OUT_DIR,
                    help=f"Output dir (default {OUT_DIR.relative_to(ROOT)})")
    ap.add_argument("--header-mm", type=float, default=8.0,
                    help="Flat header-band height mm above the HR square (default "
                         "8). Passed to hm_codegen --header_mm; the QR PNGs must "
                         "be generated with the SAME build_qr_codes.py --header-mm.")
    ap.add_argument("--size", type=float, default=60.0,
                    help="Tag bumps area mm (default 60)")
    ap.add_argument("--step", type=float, default=15.0,
                    help="Bump height step mm (default 15)")
    ap.add_argument("--base", type=float, default=5.0,
                    help="Plate base thickness mm (default 5)")
    ap.add_argument("--border", type=float, default=5.0,
                    help="Moat width mm (default 5)")
    ap.add_argument("--border-recess", type=float, default=4.0,
                    help="Moat depth mm (default 4)")
    ap.add_argument("--qr-px", type=int, default=256,
                    help="Codegen raster resolution (default 256, paired "
                         "with --smooth for clean text)")
    ap.add_argument("--seam-gap", type=float, default=0.05,
                    help="Z gap mm between white-bulk top and black-cap "
                         "bottom (default 0.05). The codegen default is "
                         "0.01 which Bambu Studio's slicer reports as "
                         "z-fighting on dense QR patterns; 0.05 is below "
                         "a single layer height (typically 0.16-0.2 mm) "
                         "so it doesn't change print appearance but gives "
                         "the slicer enough numerical room to disambiguate.")
    ap.add_argument("--cap", type=float, default=0.6,
                    help="Black-cap layer thickness mm (default 0.6 = 3 "
                         "layers at 0.2 mm). Bumped from 2 -> 3 layers: at 2 "
                         "layers the slicer left white-noise speckle in the "
                         "cap; the extra layer gives it enough depth to render "
                         "solid black.")
    ap.add_argument("--ids", type=str, default=None,
                    help="Comma-separated subset of IDs to build (e.g. "
                         "'48,99'); defaults to the full collectible set.")
    ap.add_argument("--bambu-project", type=str, default=None,
                    help="Path to a working Bambu PROJECT 3mf (with black-cap "
                         "ironing set). When given, emit Bambu PROJECT 3mfs so the "
                         "ironing applies on OPEN (no manual per-tag toggling). "
                         "Default: plain mesh 3mfs (ironing metadata present but "
                         "inert on import).")
    args = ap.parse_args()

    if not HM_CODEGEN.exists():
        print(f"hm_codegen.py not found at {HM_CODEGEN}", file=sys.stderr)
        return 2
    if not args.qr_dir.exists():
        print(f"QR dir not found: {args.qr_dir}. Run build_qr_codes.py first.",
              file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)

    if args.ids:
        wanted = set(int(x) for x in args.ids.split(","))
    else:
        wanted = set(catalog_ids())

    failed = []
    built = 0
    for id8 in sorted(wanted):
        qr_path = args.qr_dir / f"{id8}.png"
        if not qr_path.exists():
            print(f"  ID {id8}: missing QR at {qr_path}, skipping")
            failed.append(id8)
            continue

        out_path = args.out / f"{id8}.3mf"
        cmd = [
            "py", "-3", str(HM_CODEGEN),
            "--id", str(id8),
            "--qr", str(qr_path),
            "--out", str(out_path),
            "--size", str(args.size),
            "--header_mm", str(args.header_mm),
            "--step", str(args.step),
            "--base", str(args.base),
            "--border", str(args.border),
            "--border_recess", str(args.border_recess),
            "--smooth",
            "--qr_px", str(args.qr_px),
            "--seam_gap", str(args.seam_gap),
            "--cap", str(args.cap),
        ]
        if args.bambu_project:
            cmd += ["--bambu_project", str(args.bambu_project)]
        rc = subprocess.run(cmd, capture_output=True, text=True)
        if rc.returncode != 0:
            print(f"  ID {id8}: codegen FAILED")
            print(rc.stderr)
            failed.append(id8)
            continue
        built += 1
        try:
            shown = out_path.relative_to(ROOT)
        except ValueError:
            shown = out_path
        print(f"  ID {id8} -> {shown}")

    try:
        outshown = args.out.relative_to(ROOT)
    except ValueError:
        outshown = args.out
    print(f"\nBuilt {built}/{len(wanted)} tags to {outshown}")
    if failed:
        print(f"Failed: {failed}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
