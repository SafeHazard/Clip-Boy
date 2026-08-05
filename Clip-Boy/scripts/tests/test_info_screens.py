#!/usr/bin/env python3
"""
test_info_screens.py — verify Credits / Legal / About / Help content.

Each info page is shown via the `info_show` harness command (bypasses the
touch dance through the scrollable Settings page), then we read the visible
labels via `text` and assert each page contains page-specific keywords.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness


# page name -> list of keywords that should appear in the rendered text.
# Keywords are chosen to be stable identifiers of each page's purpose,
# not rotting references to specific phrasing.
PAGES = {
    "credits": ["CREDITS", "DEFCON"],
    "legal":   ["LEGAL", "BORING STUFF"],
    "about":   ["Clip-Boy"],
    # help = the expandable help-CATEGORY system (show_help renders a list, not a titled page).
    # Assert STRUCTURAL identifiers actually rendered (verified against live text() 2026-07-30):
    # "Getting Started" = help_categories[0].name; "Tour" = the guided-tour button unique to this
    # screen. NOT prose /help-review might reword. (Old keys "CLIP-BOY HELP" + "War never changes"
    # were BOTH stale: no titled header exists, and "War never changes" was IP-scrubbed from
    # firmware -- the info-screens FAIL was actually proof check_ip.py did its job.)
    "help":    ["Getting Started", "Tour"],
}


def main():
    print("=" * 60)
    print("TEST: Info Screens (Credits / Legal / About / Help)")
    print("=" * 60)

    h = Harness()
    # Make sure we're on Settings so content_obj is live
    h.nav(2, 1)  # DATA > Settings
    h.wait(200)

    passed = 0
    failed = 0
    all_errors = []

    for page, keywords in PAGES.items():
        print(f"\n--- {page} ---")
        r = h.cmd(f"info_show {page}")
        if not r.get("ok"):
            failed += 1
            all_errors.append(f"{page}: info_show failed — {r.get('error', '?')}")
            continue

        h.wait(200)  # give LVGL a tick to render labels

        text_resp = h.text()
        # text() returns {"labels": [...]}; stringify the whole dict for a loose match
        blob = str(text_resp)
        missing = [kw for kw in keywords if kw not in blob]
        if missing:
            failed += 1
            all_errors.append(f"{page}: missing keywords {missing}")
            print(f"  FAIL: missing {missing}")
        else:
            passed += 1
            print(f"  OK: found {keywords}")

        # Back to settings for next iteration (the info page's Back button
        # would normally do this, but we'll just re-nav)
        h.nav(2, 1)
        h.wait(200)

    h.close()

    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"RESULTS: {passed}/{total} passed, {failed} failed")
    if all_errors:
        print("\nFailures:")
        for e in all_errors:
            print(f"  - {e}")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
