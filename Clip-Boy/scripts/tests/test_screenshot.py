#!/usr/bin/env python3
"""test_screenshot.py — verify the harness screenshot capture writes a real BMP.

Regression test for DC34-104: the test-harness screenshot command (session
mode) read the RGB565 framebuffer off the wire but dumped the *raw* bytes to
the caller's .bmp path with no RGB565->BMP conversion, producing a 153600-byte
file with a 0x0000 magic that no image viewer could open. (CLI mode converted
correctly; only the session path the harness uses was broken.) Fix: the
session handler now runs rgb565_to_bmp() for .bmp/.png-ish paths and only dumps
raw for explicit .raw/.bin paths; read_binary got a more generous deadline.

We capture to a .bmp and assert a valid 24-bit 320x240 BMP, then capture to a
.raw and assert the raw RGB565 framebuffer (exact byte count).
"""
import sys
import os
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

# Per-port (or per-pid) output dir so CONCURRENT badge runs don't race the same fixed files.
# The fixed paths (regression_screen.bmp/.raw, regression_settings.bmp) were shared across all
# parallel processes -> they clobbered each other's captures, surfacing as a bogus "screenshot
# FAIL" that was a host-side file collision, NOT a firmware defect. Serialized T1 also avoids the
# race, but per-port namespacing keeps this test concurrency-safe on its own merits (the runner's
# target design is capped-concurrency=2). run_oracle_suite sets CLIPBOY_PORT; solo runs get a pid.
_PORT_TAG = "".join(c for c in (os.environ.get("CLIPBOY_PORT") or "") if c.isalnum()) or ("pid%d" % os.getpid())
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output", _PORT_TAG)
os.makedirs(OUT_DIR, exist_ok=True)
W, H = 320, 240
RAW_BYTES = W * H * 2            # RGB565
BMP_BYTES = 54 + W * H * 3      # 14+40 header + 24bpp pixel data


def _parse_bmp_header(path):
    with open(path, "rb") as f:
        head = f.read(54)
    magic, fsize = struct.unpack_from("<2sI", head, 0)
    px_off = struct.unpack_from("<I", head, 10)[0]
    dib, w, h, planes, bpp = struct.unpack_from("<IiiHH", head, 14)
    return magic, fsize, px_off, w, h, bpp


def test_screenshot_bmp(h):
    print("\n--- screenshot -> valid 24-bit BMP ---")
    # Land on a deterministic, content-rich screen so the capture isn't blank.
    h.nav(0, 0)  # STATS > Status
    h.wait(400)

    bmp = os.path.join(OUT_DIR, "regression_screen.bmp")
    if os.path.isfile(bmp):
        os.remove(bmp)

    # verify=True makes the harness itself assert the BM magic; we double-check
    # the full header here for dimensions / bit depth.
    r = h.screenshot(bmp)
    if not (r and r.get("ok")):
        return False, [f"screenshot command failed: {r}"]
    out = r.get("file", bmp)
    if not os.path.isfile(out):
        return False, [f"screenshot ok but {out} missing"]

    magic, fsize, px_off, bw, bh, bpp = _parse_bmp_header(out)
    actual = os.path.getsize(out)
    print(f"  {out}: {actual}B magic={magic!r} {bw}x{abs(bh)} {bpp}bpp")
    errs = []
    if magic != b"BM":
        errs.append(f"bad magic {magic!r} (raw dump masquerading as BMP?)")
    if bw != W or abs(bh) != H:
        errs.append(f"dimensions {bw}x{abs(bh)} != {W}x{H}")
    if bpp != 24:
        errs.append(f"bit depth {bpp} != 24")
    if actual != BMP_BYTES:
        errs.append(f"file size {actual} != expected {BMP_BYTES}")
    if errs:
        return False, errs
    print("  OK: openable 24-bit BMP at the requested path")
    return True, []


def test_screenshot_nonstatus(h):
    # DC34-116: the firmware snapshot used to hang on every screen EXCEPT
    # Status (lv_snapshot_take called outside the LVGL draw context). Capturing
    # a non-Status screen is what actually exercises that fix — a Status-only
    # test would pass even if the hang regressed.
    print("\n--- screenshot a non-Status screen (DC34-116 hang regression) ---")
    h.nav(2, 1)  # DATA > Settings (a heavy screen that used to hang)
    h.wait(600)
    bmp = os.path.join(OUT_DIR, "regression_settings.bmp")
    if os.path.isfile(bmp):
        os.remove(bmp)
    r = h.screenshot(bmp)
    if not (r and r.get("ok")):
        return False, [f"non-Status screenshot failed/hung: {r}"]
    magic, _, _, bw, bh, bpp = _parse_bmp_header(r.get("file", bmp))
    print(f"  {bmp}: magic={magic!r} {bw}x{abs(bh)} {bpp}bpp")
    if magic != b"BM" or bw != W or abs(bh) != H or bpp != 24:
        return False, ["non-Status capture is not a valid 24-bit 320x240 BMP"]
    print("  OK: non-Status screen captured (no hang)")
    return True, []


def test_screenshot_raw(h):
    print("\n--- screenshot -> explicit .raw still dumps RGB565 ---")
    raw = os.path.join(OUT_DIR, "regression_screen.raw")
    if os.path.isfile(raw):
        os.remove(raw)
    r = h.screenshot(raw)  # verify skipped for non-.bmp
    if not (r and r.get("ok")):
        return False, [f"raw screenshot command failed: {r}"]
    out = r.get("file", raw)
    actual = os.path.getsize(out)
    print(f"  {out}: {actual}B (expected raw RGB565 {RAW_BYTES})")
    if actual != RAW_BYTES:
        return False, [f"raw size {actual} != {RAW_BYTES} (RGB565 320x240)"]
    print("  OK: raw RGB565 framebuffer preserved for .raw paths")
    return True, []


def main():
    print("=" * 56)
    print("TEST: harness screenshot capture (DC34-104 regression)")
    print("=" * 56)
    os.makedirs(OUT_DIR, exist_ok=True)
    h = Harness()
    passed, failed, all_errs = 0, 0, []
    try:
        h.reboot_and_wait()
        h.cmd("cfg_set disp_off 5")  # screensaver=Never so the saver can't pollute a capture
        for fn in (test_screenshot_bmp, test_screenshot_nonstatus, test_screenshot_raw):
            ok, errs = fn(h)
            if ok:
                passed += 1
            else:
                failed += 1
                all_errs += errs
    finally:
        h.close()
    print("\n" + "=" * 56)
    print(f"RESULTS: {passed}/{passed + failed} passed")
    for e in all_errs:
        print("  - " + e)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
