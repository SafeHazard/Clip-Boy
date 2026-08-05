#!/usr/bin/env python3
"""
audit_ascii.py — Find non-ASCII characters inside C string literals in the
firmware source. Rendering with the pipboy font requires ASCII-only.

Usage: py -3 scripts/audit_ascii.py [--fix]

Without --fix: reports offenders.
With    --fix: applies known safe replacements (em/en dashes, smart quotes,
                ellipsis, non-breaking space) in-place, then reports anything
                that remains so a human can decide.
"""

import sys
import os
import re
import argparse

# Files to scan — only UI source that ends up rendered on the badge
UI_SOURCES = [
    "ui_nav.h",
    "ui_theme.h",
    "ui_config.h",
    "ui_collectibles.h",
    "tool_info.h",
    "Clip-Boy.ino",
]

# Unicode -> ASCII replacements considered safe to apply automatically
AUTO_FIXES = {
    "\u2014": " - ",   # em-dash
    "\u2013": "-",      # en-dash
    "\u2018": "'",      # left single quote
    "\u2019": "'",      # right single quote / apostrophe
    "\u201c": '"',      # left double quote
    "\u201d": '"',      # right double quote
    "\u00a0": " ",      # non-breaking space
    "\u2026": "...",    # ellipsis
}

# Regex matches a C double-quoted string literal, handling backslash escapes.
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')

# Patterns that LVGL's word-wrap breaks badly. Inside a line we might render
# "2.4" as "2." + "4" because LVGL treats period as a word boundary. These are
# reported so the author can reword (avoiding the pattern is the only fix —
# a non-breaking space is Unicode, which would fail the ASCII gate).
BAD_WRAP_PATTERNS = [
    (re.compile(r"\b\d+\.\d+\s*[A-Za-z]"), "digit.digit followed by letters (e.g. '2.4GHz')"),
    (re.compile(r"\b\d+\.\d+\.\d+"),         "version-like dotted numbers (e.g. '1.2.3')"),
    (re.compile(r"\b[A-Z]\.[A-Z]\.[A-Z]"),   "initialism with dots (e.g. 'U.S.A.')"),
    # Leading-period extensions ("a .pcap file") wrap so the dot lands at
    # the start of the next line -- looks like a malformed sentence.
    (re.compile(r"\s\.[a-z]{2,6}\b"),        "leading-period file ext (e.g. '.pcap' after a space)"),
]


def scan_file(path, fix=False):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    original = text
    if fix:
        for u, a in AUTO_FIXES.items():
            text = text.replace(u, a)
        if text != original:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(text)

    issues = []
    wrap_issues = []
    for m in STRING_RE.finditer(text):
        literal = m.group(0)
        line_no = text[:m.start()].count("\n") + 1
        bad = [c for c in literal if ord(c) > 127]
        if bad:
            issues.append({
                "file": path,
                "line": line_no,
                "chars": sorted(set((hex(ord(c)), c) for c in bad)),
                "preview": literal[:80].replace('"', '\\"'),
            })
        # Wrap heuristics — skip format strings (contain %) and very short literals
        if "%" in literal or len(literal) < 20:
            continue
        for pat, reason in BAD_WRAP_PATTERNS:
            m2 = pat.search(literal)
            if m2:
                wrap_issues.append({
                    "file": path,
                    "line": line_no,
                    "match": m2.group(0),
                    "reason": reason,
                    "preview": literal[:80].replace('"', '\\"'),
                })
                break
    return issues, wrap_issues


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true", help="Apply safe auto-replacements in-place")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    all_ascii = []
    all_wrap = []
    for rel in UI_SOURCES:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        ascii_issues, wrap_issues = scan_file(path, fix=args.fix)
        all_ascii.extend(ascii_issues)
        all_wrap.extend(wrap_issues)

    rc = 0

    if all_ascii:
        rc = 1
        print(f"Non-ASCII in {len(all_ascii)} string literal(s):")
        for i in all_ascii:
            chars_str = ", ".join(h for h, _ in i["chars"])
            try:
                print(f"  {os.path.relpath(i['file'])}:{i['line']} [{chars_str}] {i['preview']}")
            except UnicodeEncodeError:
                safe = i["preview"].encode("ascii", "replace").decode("ascii")
                print(f"  {os.path.relpath(i['file'])}:{i['line']} [{chars_str}] {safe}")
    else:
        print("ASCII-only: OK")

    if all_wrap:
        # Wrap warnings don't fail the check (sometimes they're acceptable —
        # a version number deep in legal text is fine). Just flag for review.
        print(f"\nPotential bad wraps in {len(all_wrap)} string(s):")
        for w in all_wrap:
            print(f"  {os.path.relpath(w['file'])}:{w['line']} [{w['match']}] ({w['reason']})")
            print(f"    {w['preview']}")
    else:
        print("Wrap heuristics: OK")

    return rc


if __name__ == "__main__":
    sys.exit(main())
