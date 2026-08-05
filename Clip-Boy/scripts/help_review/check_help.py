#!/usr/bin/env python3
"""Pre-build CI guard for the Clip-Boy Help system (deterministic, no LLM).

Runs every build (fast). Three checks:
  1. DENYLIST  - scan Help + tool_info copy for profanity/slurs that must never
                 ship. Any hit FAILS the build.
  2. PATHS     - tutorial menu paths ("Tools > <Category> > ...") must name a
                 LIVE tool category. Catches the stale-path class of bug the
                 red-team panel found after the DETECT-LED taxonomy change.
  3. STALENESS - if the Help corpus changed since the last recorded review
                 (docs/help-review/last-reviewed.json), WARN that a full
                 /help-review (expert panel) is due. The heavy multi-agent
                 review is on-demand; this just enforces it gets re-run when
                 Help actually changes.

Exit codes: 0 = clean (warnings allowed). 1 = a FAIL check tripped.
`--strict` promotes the staleness WARNING to a FAIL (use in release gating).

Usage: py -3 scripts/help_review/check_help.py [--strict]
"""
import json
import os
import re
import sys

HERE = os.path.dirname(__file__)
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
UI_NAV = os.path.join(ROOT, "ui_nav.h")
TOOL_INFO = os.path.join(ROOT, "tool_info.h")
STAMP = os.path.join(ROOT, "docs", "help-review", "last-reviewed.json")

sys.path.insert(0, HERE)
from gen_help_artifact import parse_help, corpus_sha  # noqa: E402

# Conservative, high-precision denylist (word-boundary, case-insensitive).
# Keep it to terms that should NEVER appear in shipping copy; the nuanced
# legal/tone review is the agent panel's job, not this gate.
DENY = [
    r"fuck", r"shit", r"bitch", r"cunt", r"asshole", r"\bdick\b", r"\bcock\b",
    r"nigger", r"nigga", r"faggot", r"\bfag\b", r"retard", r"\bspic\b",
    r"\bchink\b", r"\bkike\b", r"tranny", r"\bdyke\b",
]
DENY_RE = re.compile("|".join(DENY), re.I)


def read(p):
    return open(p, encoding="utf-8", errors="replace").read()


def live_categories(src):
    """Category display names from tool_categories[]."""
    m = re.search(r'tool_categories\[\]\s*=\s*\{(.*?)\};', src, re.S)
    if not m:
        return None
    # entries look like { id, "Name", cat_array, count, ... } -> first quoted
    # string per { ... } row is the display name.
    names = set()
    for row in re.finditer(r'\{([^{}]*)\}', m.group(1)):
        q = re.search(r'"((?:[^"\\]|\\.)*)"', row.group(1))
        if q:
            names.add(q.group(1))
    return names


def main():
    strict = "--strict" in sys.argv
    src = read(UI_NAV)
    cats = parse_help(src)
    fails, warns = [], []

    # --- 1. denylist over Help + tool_info copy ---
    corpus_text = []
    for c in cats:
        for it in c["items"]:
            corpus_text.append((c["name"] + " > " + it["title"], it["text"]))
    if os.path.exists(TOOL_INFO):
        for q in re.finditer(r'"((?:[^"\\]|\\.)*)"', read(TOOL_INFO)):
            corpus_text.append(("tool_info", q.group(1)))
    # The collectibles catalog is now user-facing shipped content (~95 titles +
    # descriptions baked into the firmware via data/collectibles.csv). Scan its
    # field values too so profanity/slurs can't slip into a release uncaught.
    csv_path = os.path.join(ROOT, "data", "collectibles.csv")
    if os.path.exists(csv_path):
        import csv as _csv
        with open(csv_path, encoding="utf-8", errors="replace", newline="") as cf:
            for r in _csv.DictReader(cf):
                cid = (r.get("ID") or "?").strip()
                for col in ("Title", "Source", "Description"):
                    val = (r.get(col) or "").strip()
                    if val:
                        corpus_text.append((f"collectible {cid} {col}", val))
    # The README's "Known issues" section is user-facing copy making checkable claims about the
    # firmware -- the same category as the on-badge Help, and it drifts the same way (a 2026-07-26
    # fix invalidated a section stating two conditions were indistinguishable). It is part of the
    # staleness corpus (see corpus_sha), so scan it for denylisted terms as well; otherwise the two
    # halves of "is this text reviewed" would disagree about what the text IS.
    from gen_help_artifact import readme_known_issues  # noqa: E402
    _ri = readme_known_issues()
    if _ri.startswith("\x02"):
        warns.append(f"README Known issues section not readable ({_ri[1:]}) -- it is part of the "
                     f"review corpus, so this also means the staleness stamp cannot match")
    else:
        for para in [p.strip() for p in _ri.split("\n\n") if p.strip()]:
            corpus_text.append(("README Known issues", para))

    for loc, txt in corpus_text:
        hit = DENY_RE.search(txt)
        if hit:
            fails.append(f"DENYLIST: '{hit.group(0)}' in {loc!r}")

    # --- 2. tutorial menu-path lint ---
    live = live_categories(src)
    if live is None:
        warns.append("PATHS: could not locate tool_categories[]; lint skipped")
    else:
        seen = set()
        for c in cats:
            for it in c["items"]:
                for m in re.finditer(r'Tools >\s*([A-Za-z0-9/ ]+?)\s*(?:>|[,.])',
                                     it["text"]):
                    catname = m.group(1).strip()
                    if catname and catname not in live and catname not in seen:
                        seen.add(catname)
                        fails.append(
                            f"PATHS: tutorial cites Tools > '{catname}' "
                            f"but no such tool category exists "
                            f"(live: {', '.join(sorted(live))})")

    # --- 3. staleness ---
    cur = corpus_sha(cats)
    if os.path.exists(STAMP):
        rec = json.load(open(STAMP, encoding="utf-8"))
        if rec.get("corpus_sha") != cur:
            msg = ("STALENESS: Help corpus changed since the last review "
                   f"({rec.get('reviewed_utc','?')}). Run the /help-review skill "
                   "to re-review + regenerate the artifact.")
            (fails if strict else warns).append(msg)
    else:
        # No stamp = corpus was NEVER reviewed. Under --strict (release gating)
        # that must FAIL, not warn -- otherwise a never-reviewed corpus ships.
        (fails if strict else warns).append(
            "STALENESS: no last-reviewed.json; run /help-review once.")

    for w in warns:
        print(f"[help-check] WARN  {w}")
    for fmsg in fails:
        print(f"[help-check] FAIL  {fmsg}")
    if fails:
        print(f"[help-check] FAILED ({len(fails)} fail, {len(warns)} warn)")
        sys.exit(1)
    print(f"[help-check] OK ({len(warns)} warn) "
          f"- {len(cats)} cats, {sum(len(c['items']) for c in cats)} entries")


if __name__ == "__main__":
    main()
