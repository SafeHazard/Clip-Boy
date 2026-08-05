#!/usr/bin/env python3
"""Report alpha/alpha patterns inside string literals.

LVGL's word-wrap breaks on: space, comma, period, ?, ;, !, -, _
It does NOT break on /, so tokens like 'routers/hotspots' are atomic
and can force a short line if they're long.
"""
import os
import re
import sys

TARGETS = ["ui_nav.h", "tool_info.h", "Clip-Boy.ino", "ui_theme.h", "ui_config.h"]
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
SLASH_PAIR = re.compile(r"[A-Za-z]{2,}/[A-Za-z]{2,}(?:/[A-Za-z]{2,})*")

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    found = 0
    for rel in TARGETS:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        for m in STRING_RE.finditer(text):
            literal = m.group(0)
            if "%" in literal:
                continue
            hits = SLASH_PAIR.findall(literal)
            if not hits:
                continue
            line = text[: m.start()].count("\n") + 1
            # Only report tokens >=10 chars (shorter ones rarely hurt wrapping)
            long_hits = [h for h in hits if len(h) >= 10]
            if not long_hits:
                continue
            found += 1
            preview = literal[:90].replace('"', '\\"')
            print(f"{rel}:{line}  {long_hits}")
            print(f"  {preview}")
    if found == 0:
        print("No long alpha/alpha slash tokens found.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
