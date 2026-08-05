#!/usr/bin/env python3
"""
test_manual_entry.py — manual-entry / verify-flow engine + two regression checks.

Mirrors the feature/manual-entry-grid work:
  * decodeUserGrid round-trip — the host encoder (scripts/id_to_code.user_view, what a
    post-con sharer would tap) must decode to the same id ON THE DEVICE. Also catches any
    host<->firmware encoder-convention drift.
  * invalid grid is rejected (ok:false).
  * manual_grid opens (blank + prefilled) without erroring — the entry points + the
    collapsed one-screen verify path build cleanly.
  * C1 regression — audio_suspend() with EVERY render source active must not reboot.
  * #6 regression — the radio announce-mask stays synced (no spurious "new station").

Serial (STX) session via harness. audio_suspend_test/manual_decode/radio_announce_dump
were given STX+JSON results so the bridge can read them (it skips plain Serial output).
"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))  # scripts/
from harness import Harness


def _bits(uv):
    """4x4 user-view grid -> 16-char row-major '0'/'1' string (manual_decode format)."""
    return "".join("1" if uv[r][c] else "0" for r in range(4) for c in range(4))


def main():
    passed = 0
    failed = 0
    errors = []

    print("=" * 60)
    print("TEST: Manual entry / verify flow + audio-suspend + radio regressions")
    print("=" * 60)

    # Host encoder (same one id_to_code.py hands sharers). Optional: if the sim deps
    # aren't importable, skip the round-trip rather than hard-fail the whole test.
    try:
        from id_to_code import user_view
        have_encoder = True
    except Exception as e:
        have_encoder = False
        print(f"  (encoder import failed: {e} -- skipping round-trip section)")

    h = Harness()
    try:
        # [1] host-encode -> device-decode round-trip across a spread of ids (incl. the
        #     reliability-remapped 82/108/112/118/127).
        if have_encoder:
            print("\n[1] manual_decode round-trip (host encode -> device decode)...")
            for idv in [1, 5, 20, 37, 55, 82, 100, 108, 112, 118, 127]:
                try:
                    bits = _bits(user_view(idv))
                except Exception as e:
                    failed += 1
                    errors.append(f"host encode id {idv}: {e}")
                    print(f"  FAIL: host encode id {idv}: {e}")
                    continue
                r = h.cmd(f"manual_decode {bits}")
                if r.get("ok") and r.get("id") == idv:
                    passed += 1
                    print(f"  OK: id {idv} round-trips")
                else:
                    failed += 1
                    errors.append(f"manual_decode id {idv} -> {r}")
                    print(f"  FAIL: id {idv} -> {r}")

        # [2] invalid pattern rejected
        print("\n[2] invalid pattern rejected...")
        r = h.cmd("manual_decode 0000000000000000")
        if r.get("ok") is False:
            passed += 1
            print("  OK: all-flat grid rejected")
        else:
            failed += 1
            errors.append(f"all-flat not rejected: {r}")
            print(f"  FAIL: all-flat not rejected: {r}")

        # [3] manual_grid opens (blank + prefilled)
        print("\n[3] manual_grid opens...")
        cases = [("", "blank")]
        if have_encoder:
            cases.append((_bits(user_view(1)), "prefilled id=1"))
        for arg, label in cases:
            r = h.cmd(f"manual_grid {arg}".strip())
            if r.get("ok"):
                passed += 1
                print(f"  OK: grid opens ({label})")
            else:
                failed += 1
                errors.append(f"manual_grid {label}: {r}")
                print(f"  FAIL: manual_grid {label}: {r}")
        h.nav(1, 1)  # leave the grid overlay -> Collectibles

        # [4] C1 regression: audio_suspend with each render source active -> no reboot.
        #     (A reboot kills the JSON reply -> the harness times out -> this FAILs.)
        print("\n[4] audio_suspend_test (C1 I2S-race regression)...")
        r = h.cmd("audio_suspend_test")
        if r.get("ok") and r.get("reboot") is False:
            passed += 1
            print(f"  OK: {r.get('sources')} sources survived suspend/resume")
        else:
            failed += 1
            errors.append(f"audio_suspend_test: {r}")
            print(f"  FAIL: audio_suspend_test: {r}")

        # [5] #6 regression: announce-mask synced to gated-unlocked stations.
        print("\n[5] radio_announce_dump (#6 re-announce regression)...")
        r = h.cmd("radio_announce_dump")
        if r.get("synced") is True:
            passed += 1
            print(f"  OK: announced={r.get('announced')} gated_unlocked={r.get('gated_unlocked')} synced")
        else:
            failed += 1
            errors.append(f"radio not synced: {r}")
            print(f"  FAIL: radio not synced: {r}")

    finally:
        h.close()

    print("\n" + "=" * 60)
    print(f"RESULT: {passed} passed, {failed} failed")
    for e in errors:
        print(f"  - {e}")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
