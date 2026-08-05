#!/usr/bin/env python3
"""id_to_code.py -- Clip-Boy collectible ID -> hand-entry tag pattern.

POST-CON companion tool. On the badge, DATA > ITEMS > Collectibles > (scan screen) >
MANUAL ENTRY lets you hand-enter a tag's 4x4 raised-bump pattern. This prints that
pattern for any collectible so completionists can fill tags they never scanned.

The grid you see here is the USER'S VIEW -- exactly how the cells are laid out on the
badge's manual-entry screen (hold the badge normally). RAISE the cells shown as [#]
(tap them so they're filled/domed); leave [.] flat. The 4 corners are fixed structure
(3 raised + 1 flat, bottom-left) and can't be tapped -- they're shown for orientation.

Usage:
  py -3 scripts/id_to_code.py 47            # one id
  py -3 scripts/id_to_code.py "WOPR"        # by name (substring, case-insensitive)
  py -3 scripts/id_to_code.py --all         # every collectible in the catalog
  py -3 scripts/id_to_code.py --missing 1,2,5,47   # ids you're MISSING (inverse of --have)
  py -3 scripts/id_to_code.py --have 3,4,6         # ids you HAVE -> prints the rest

No badge needed. Pure catalog + SECDED math; self-verifies each pattern round-trips.
"""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "hr_spec"))
from secded import encode, decode          # SECDED(12,7): id<->codeword
from pattern_hardness import grid_for, CELL_ORDER

REPO = os.path.dirname(HERE)
CSV_PATH = os.path.join(REPO, "data", "collectibles.csv")


def user_view(idv):
    """Decoder-space grid_for(id), X-mirrored to the badge's user-view (what you tap)."""
    g = grid_for(idv)
    return [[g[r][3 - c] for c in range(4)] for r in range(4)]


def verify(idv):
    """Round-trip: encode -> decode == id. Guards against a bad catalog id."""
    try:
        return decode(encode(idv))[0] == idv     # decode -> (id, status)
    except Exception:
        return False


def render(idv, title):
    uv = user_view(idv)
    ok = verify(idv)
    corners = {(0, 0), (0, 3), (3, 0), (3, 3)}
    print(f"\n  ID {idv:<4} {title}")
    if not ok:
        print("  !! this id does not round-trip -- not a valid tag code; skipping")
        return
    print("  (hold the badge normally; RAISE [#], leave [.] flat; corners are fixed)")
    for r in range(4):
        row = "   "
        for c in range(4):
            cell = "[#]" if uv[r][c] else "[.]"
            if (r, c) in corners:
                cell = "(#)" if uv[r][c] else "(.)"   # () = fixed corner, don't tap
            row += " " + cell
        print(row)
    raised = sum(uv[r][c] for r in range(4) for c in range(4)
                 if (r, c) not in corners)
    print(f"   -> raise {raised} of the 12 middle cells")


def load_catalog():
    cat = {}
    with open(CSV_PATH, newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        next(r, None)                       # header ID,Title,...
        for row in r:
            if row and row[0].strip().isdigit():
                cat[int(row[0])] = (row[1].strip() if len(row) > 1 else "")
    return cat


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cat = load_catalog()
    arg = sys.argv[1]

    if arg == "--all":
        for idv in sorted(cat):
            render(idv, cat[idv])
    elif arg in ("--missing", "--have"):
        if len(sys.argv) < 3:
            print("give a comma list of ids, e.g. --have 1,2,5")
            return 2
        given = {int(x) for x in sys.argv[2].replace(" ", "").split(",") if x.isdigit()}
        want = (set(cat) - given) if arg in ("--missing", "--have") else set()
        # --missing <ids-you-have> and --have <ids-you-have> both mean "print the rest"
        for idv in sorted(want):
            render(idv, cat[idv])
        print(f"\n  {len(want)} tag(s) to hand-enter.")
    elif arg.isdigit():
        idv = int(arg)
        render(idv, cat.get(idv, "(not in catalog)"))
    else:
        needle = arg.lower()
        hits = [(i, t) for i, t in sorted(cat.items()) if needle in t.lower()]
        if not hits:
            print(f"no collectible name matches '{arg}'")
            return 1
        for idv, title in hits:
            render(idv, title)
    return 0


if __name__ == "__main__":
    sys.exit(main())
