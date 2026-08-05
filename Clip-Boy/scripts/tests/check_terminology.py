#!/usr/bin/env python3
"""Source-lint guard against weaponization-framing regressions.

Scans FIRST-PARTY source (our code, not the vendored upstream Marauder library)
for the framing words the badge's legal posture forbids: 'weapon', 'victim',
and 'attack' used as a characterization. Genuine technical terms are allowlisted
(dictionary attack, denial-of-service, etc.). Run in CI / pre-commit.

Exit 0 = clean, 1 = violations found.
"""
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# First-party files only. The vendored libs/ upstream Marauder code is
# attributed third-party and is handled by build-gating, not renaming.
SCAN_FILES = [
    "Clip-Boy.ino", "ui_nav.h", "ui_theme.h", "ui_config.h", "tool_info.h",
    "test_harness.h", "ui_collectibles.h", "audio_driver.h", "neopixel_driver.h",
]

# Hard-forbidden substrings (case-insensitive) - never acceptable in our code.
# `\bhack\b` matches the bare VERB/imperative ("hack responsibly", "go hack",
# "hack the planet") which reads as encouragement to attack. It deliberately
# does NOT match the descriptive DEFCON-culture nouns "hacker", "hacking",
# "hackathon" (the trailing letters defeat the trailing \b) - those stay legal
# (e.g. "hacker con", "our hacking suite").
FORBIDDEN = [r"\bweapon", r"\bvictim", r"\bhack\b"]

# Glamorizing phrases that use the -ing/-er forms `\bhack\b` can't catch.
FORBIDDEN_PHRASES = ["happy hacking", "hack the planet", "hack away"]

# 'attack' is allowed ONLY as part of these genuine technical terms.
ATTACK_ALLOW = [
    "dictionary attack",     # the actual cryptographic term
    "an \"attack\"",         # the policy comment that names the forbidden word
    "as an attack",          # ditto, if present
]


def main():
    violations = []
    for rel in SCAN_FILES:
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, 1):
                low = line.lower()
                for pat in FORBIDDEN:
                    if re.search(pat, low):
                        violations.append(f"{rel}:{lineno}: forbidden {pat!r}: {line.strip()[:80]}")
                for phrase in FORBIDDEN_PHRASES:
                    if phrase in low:
                        violations.append(f"{rel}:{lineno}: forbidden phrase {phrase!r}: {line.strip()[:80]}")
                if "attack" in low:
                    if not any(a in low for a in ATTACK_ALLOW):
                        violations.append(f"{rel}:{lineno}: 'attack' framing: {line.strip()[:80]}")

    print("=" * 56)
    if violations:
        print(f"FAIL: {len(violations)} terminology violation(s) in first-party source:")
        for v in violations:
            print("  - " + v)
        return 1
    print(f"PASS: {len(SCAN_FILES)} first-party files free of weaponization framing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
