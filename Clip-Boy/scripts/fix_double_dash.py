#!/usr/bin/env python3
"""Collapse '  -  ' (double-space dash) to ' - ' (single-space dash) in UI sources."""
import os, sys

TARGETS = ["ui_nav.h", "tool_info.h", "Clip-Boy.ino", "ui_theme.h", "ui_config.h"]

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
total = 0
for rel in TARGETS:
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        continue
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    before = text.count("  -  ")
    if before == 0:
        continue
    text = text.replace("  -  ", " - ")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"  {rel}: fixed {before} occurrences")
    total += before
print(f"Total: {total}")
