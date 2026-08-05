#!/usr/bin/env python3
"""Guard-adjacency analysis for HR anchor tags (DC34-155).

Hardware finding (Jul 2 2026): the tags that misdecode (16, 88) are NOT
distinguished by sparseness or bumps-near-the-BL-anchor (ID 73 works with the
MOST BL-adjacent bumps). What they share is bumps in the cells 8-connected to
the GUARD corner (3,3): (2,2),(2,3),(3,2). The guard MUST read flat (it is the
rotation key); adjacent bumps bleed into it -> guard reads near -> the miss/
rotation solve fails -> misdecode. (The user first spotted this in the
X-mirrored VIEW frame as (2,1)/(3,1)/(2,0); those are the column-mirror of the
guard neighbors.)

Correlation on the 4 IDs we have hardware data for:
    ID 16  guard-adj=2  -> fails
    ID 88  guard-adj=2  -> fails
    ID 42  guard-adj=1  -> marginal
    ID 73  guard-adj=1  -> works
=> guard-adj>=2 predicts failure; guard-adj==0 is the safe zone.

Reads the authoritative id->grid table so we work in the DECODE frame (never
tag transcriptions, which carry the X-mirror). Prints the AVOID / SAFE / MARGINAL
partitions so physical-tag collectibles can be mapped to guard-clean IDs.
"""
import os, re

TABLE = os.path.join(os.path.dirname(__file__), "omnitag_id_table.txt")
GUARD_NB = [(2, 2), (2, 3), (3, 2)]          # cells 8-connected to guard (3,3)
CELL_ORDER = [(0,1),(0,2),(1,0),(1,1),(1,2),(1,3),(2,0),(2,1),(2,2),(2,3),(3,1),(3,2)]


def load_grids():
    grids = {}
    for line in open(TABLE):
        m = re.match(r"\s*(\d+)\s*\|\s*([01]{12})\s*\|\s*([01/]+)", line)
        if m:
            rows = m.group(3).split("/")
            grids[int(m.group(1))] = [[int(c) for c in r] for r in rows]
    return grids


def guard_adj(g):
    return sum(g[r][c] for r, c in GUARD_NB)


def bits(g):
    return [g[r][c] for r, c in CELL_ORDER]


def hamming(a, b):
    return sum(x != y for x, y in zip(bits(a), bits(b)))


def main():
    grids = load_grids()
    ids = sorted(grids)
    safe    = [i for i in ids if guard_adj(grids[i]) == 0]   # predict reliable
    margin  = [i for i in ids if guard_adj(grids[i]) == 1]   # 73 ok / 42 marginal
    avoid   = [i for i in ids if guard_adj(grids[i]) >= 2]   # predict fail (16/88 here)

    print(f"total IDs: {len(ids)}")
    print(f"SAFE    (guard-adj==0, {len(safe)}): {safe}")
    print(f"MARGINAL(guard-adj==1, {len(margin)}): {margin}")
    print(f"AVOID   (guard-adj>=2, {len(avoid)}): {avoid}")
    print("\nClosest SAFE control for each known-bad id (Hamming on 12 data bits):")
    for tgt in (16, 88):
        cand = sorted(safe, key=lambda i: hamming(grids[i], grids[tgt]))[:3]
        print(f"  vs {tgt}: " + ", ".join(f"{i}(d={hamming(grids[i], grids[tgt])})" for i in cand))


if __name__ == "__main__":
    main()
