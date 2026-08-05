#!/usr/bin/env python3
"""capture_ui_tour.py — Capture all 9 UI tabs for reference frames.

Two modes:
  (default) single-session: nav -> wait -> screenshot -> verify -> settle
            Fast (~45s total). Matches test_navigation.py pattern.

  --reboot-between: reboot the badge between each screen. Slow (~4 min)
            but bulletproof — each screenshot happens from a freshly-
            booted state, which is proven to work.

Both modes convert raw RGB565 dumps to PNG at the end.

Usage:
    py -3 scripts/capture_ui_tour.py
    py -3 scripts/capture_ui_tour.py --reboot-between
    py -3 scripts/capture_ui_tour.py --port COM8 --outdir shots
"""

import sys
import os
import argparse
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(SCRIPT_DIR, "tests"))
from harness import Harness  # noqa: E402

SCREENS = [
    (0, 0, "stats-status"),
    (0, 1, "stats-leet"),
    (0, 2, "stats-radiation"),
    (1, 0, "items-tools"),
    (1, 1, "items-collectibles"),
    (1, 2, "items-saos"),
    (2, 0, "data-leds"),
    (2, 1, "data-settings"),
    (2, 2, "data-theremin"),
]

EXPECTED_RAW_SIZE = 320 * 240 * 2  # 153,600 bytes
WAIT_AFTER_NAV_MS     = 300   # let LVGL settle after tab rebuild
WAIT_AFTER_SHOT_MS    = 800   # let firmware complete serial flush + cleanup
REBOOT_SETTLE_SECONDS = 25    # match harness default


def rgb565_raw_to_png(raw_path, png_path, w=320, h=240):
    from PIL import Image
    with open(raw_path, "rb") as f:
        data = f.read()
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * 2
            val = data[off] | (data[off + 1] << 8)
            r = ((val >> 11) & 0x1F) << 3
            g = ((val >> 5) & 0x3F) << 2
            b = (val & 0x1F) << 3
            px[x, y] = (r, g, b)
    img.save(png_path)


def verify_raw(path):
    """Check the raw file landed correctly. Returns (ok, message)."""
    if not os.path.exists(path):
        return False, "file not written"
    sz = os.path.getsize(path)
    if sz != EXPECTED_RAW_SIZE:
        return False, f"size {sz} != expected {EXPECTED_RAW_SIZE}"
    return True, f"{sz} bytes"


def capture_once(h, div, tab, raw_path):
    """nav -> wait -> screenshot -> verify. Returns True on success."""
    r = h.nav(div, tab)
    if not r.get("ok"):
        print(f"  ERR nav: {r}")
        return False

    h.wait(WAIT_AFTER_NAV_MS)

    s = h.state()
    if s.get("ok"):
        print(f"  state: {s['div_name']} > {s['tab_name']}")

    r = h.screenshot(raw_path)
    if not r.get("ok"):
        print(f"  ERR screenshot: {r}")
        return False

    h.wait(WAIT_AFTER_SHOT_MS)

    ok, msg = verify_raw(raw_path)
    if not ok:
        print(f"  ERR verify: {msg}")
        return False
    print(f"  wrote {raw_path} ({msg})")
    return True


def run_single_session(outdir):
    print(f"[capture] single-session mode")
    print(f"[capture] opening session...")
    h = Harness(skip_boot=True)
    results = []
    try:
        for idx, (div, tab, name) in enumerate(SCREENS, start=1):
            tag = f"{idx:02d}-{name}"
            raw = os.path.join(outdir, f"{tag}.raw")
            print(f"[capture] nav {div} {tab} ({name})")
            ok = capture_once(h, div, tab, raw)
            if ok:
                results.append((raw, os.path.join(outdir, f"{tag}.png")))
    finally:
        h.close()
    return results


def run_reboot_between(outdir):
    print(f"[capture] reboot-between mode (~4 min total)")
    results = []
    for idx, (div, tab, name) in enumerate(SCREENS, start=1):
        tag = f"{idx:02d}-{name}"
        raw = os.path.join(outdir, f"{tag}.raw")
        print(f"\n[capture] ({idx}/{len(SCREENS)}) nav {div} {tab} ({name})")
        print(f"  [reboot] opening session + waiting for boot...")
        h = Harness(skip_boot=True)
        try:
            ok = capture_once(h, div, tab, raw)
            if ok:
                results.append((raw, os.path.join(outdir, f"{tag}.png")))
            # Reboot kills serial before sending a response; swallow timeout
            try:
                h.cmd("reboot")
            except Exception:
                pass
        finally:
            try:
                h.close()
            except Exception:
                pass
        if idx < len(SCREENS):
            print(f"  [reboot] settling {REBOOT_SETTLE_SECONDS}s...")
            time.sleep(REBOOT_SETTLE_SECONDS)
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--outdir", default="shots")
    ap.add_argument("--reboot-between", action="store_true",
                    help="Reboot between each screen (slow, most reliable)")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    if args.reboot_between:
        results = run_reboot_between(args.outdir)
    else:
        results = run_single_session(args.outdir)

    if not results:
        print("\n[done] no frames captured")
        return

    print(f"\n[convert] RGB565 -> PNG for {len(results)} frame(s)...")
    for raw, png in results:
        try:
            rgb565_raw_to_png(raw, png)
            print(f"  {png}")
        except Exception as e:
            print(f"  ERR converting {raw}: {e}")

    print(f"\n[done] {len(results)} frame(s) in {args.outdir}/")


if __name__ == "__main__":
    main()
