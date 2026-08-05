#!/usr/bin/env python3
"""Render a perf-sweep log as a markdown per-tool table, with real tool names.

The sweep logs record pages as `Category[index]` only. Hand-mapping ~100 rows to
names is exactly where a transcription error hides, so the names are PARSED from
ui_nav.h (`cat_*[]` + `tool_categories[]`) at render time -- if a tool is renamed
or reordered, this table follows automatically.

Everything else (verdict, DRAM delta, FPS) is copied VERBATIM from the log. This
script does not recompute any measurement; a table that recalculated the numbers
would be verifying a copy of the sweep, not the sweep.

Usage:
    py -3 scripts/tests/render_perf_table.py <sweep.log> [title] > table.md

Note the res34rch caveat when reading the output: `OK` on an active-transmit tool
means "started and stopped cleanly", NOT "radiated". Active TX must be verified
with an EXTERNAL receiver (see CLAUDE.md, Critical Integration Notes).
"""
import os
import re
import sys
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
NAV = os.path.normpath(os.path.join(HERE, "..", "..", "ui_nav.h"))

ROW = re.compile(
    r"^\s{2}(?P<cat>[A-Za-z/ ]+?)\[(?P<idx>\d+)\]\s+"
    r"(?P<verdict>OK|DID-NOT-START|ONESHOT-NOT-FIRED|FAIL|LEAK)\s+"
    r"DRAM (?P<d0>\d+)->(?P<d1>\d+) \((?P<dd>[-+]\d+)\)\s+"
    r"FPS (?P<fmin>\d+)/(?P<fmax>\d+)/(?P<favg>\d+) n=(?P<n>\d+)")

MARK = {
    "OK": "OK",
    "DID-NOT-START": "*not startable*",
    "ONESHOT-NOT-FIRED": "**NOT EXERCISED**",
}


def parse_names(nav_path):
    """{display category name: [tool names in index order]} straight from the source."""
    with open(nav_path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    arrays = {}
    for m in re.finditer(r"static const ToolItem (cat_\w+)\[\] = \{(.*?)\n\};", src, re.S):
        # Entry names are the first string literal on an entry's opening line. Descriptions
        # may wrap across lines, so anchor to the `{ "` that starts an entry.
        arrays[m.group(1)] = re.findall(r"^\s*\{\s*\"([^\"]+)\"", m.group(2), re.M)

    blk = re.search(r"static const ToolCategory tool_categories\[\] = \{(.*?)\n\};", src, re.S)
    if not blk:
        raise SystemExit("cannot test: tool_categories[] not found in " + nav_path)

    names = {}
    for m in re.finditer(r"\{\s*\d+,\s*\"([^\"]+)\",\s*(cat_\w+)", blk.group(1)):
        disp, arr = m.group(1), m.group(2)
        if arr not in arrays:
            raise SystemExit("cannot test: %s referenced but not parsed" % arr)
        names[disp] = arrays[arr]
    return names


def main(argv):
    # Windows stdout is cp1252 and this emits U+25B8 / U+2014. Without this the script
    # dies mid-table with a UnicodeEncodeError -- the same defect that once silenced 9 of
    # 16 checks in check_security_regressions.py.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if len(argv) < 2:
        raise SystemExit(__doc__)
    log_path = argv[1]
    title = argv[2] if len(argv) > 2 else os.path.basename(log_path)

    names = parse_names(NAV)

    rows = []
    with open(log_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = ROW.match(line.rstrip("\n"))
            if m:
                rows.append(m.groupdict())

    if not rows:
        # An empty table must never read as "nothing went wrong".
        raise SystemExit("cannot test: no sweep rows matched in %s" % log_path)

    unmapped = 0
    print("### %s\n" % title)
    counts = collections.Counter(r["verdict"] for r in rows)
    print("`%d` pages visited — " % len(rows)
          + ", ".join("**%d %s**" % (v, k) for k, v in counts.most_common()) + "\n")
    print("| # | tool | verdict | DRAM delta | FPS min/max/avg |")
    print("|---|---|---|---|---|")
    for i, r in enumerate(rows, 1):
        cat, idx = r["cat"].strip(), int(r["idx"])
        try:
            nm = names[cat][idx]
        except (KeyError, IndexError):
            nm = "?? UNMAPPED"
            unmapped += 1
        print("| %d | %s ▸ %s | %s | %s | %s/%s/%s |"
              % (i, cat, nm, MARK.get(r["verdict"], r["verdict"]),
                 r["dd"], r["fmin"], r["fmax"], r["favg"]))
    print()

    if unmapped:
        # Fail loudly: a silently mislabelled table is worse than no table.
        print("**%d rows could not be mapped to a tool name — the log and ui_nav.h disagree.**"
              % unmapped)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
