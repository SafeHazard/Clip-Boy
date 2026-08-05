#!/usr/bin/env python3
"""Spatial-frequency ("checkerboard") hardness of every HR anchor ID (0-127).

Hardware finding (Jul 2 2026, after the graded-ring + pinned-guard rewrite): the
pose is now solved correctly for essentially every tag, but a few still misdecode
because their DATA cells alias -- a 4x4 pattern at ~1.75 sensor-zones/cell can't
resolve a cell that differs from all its neighbors (an "isolated" cell). Observed:
    id 43  iso=7  -> FAIL      (nearly half its cells isolated)
    id 37  iso=5  -> slow
    id 16  iso=3  -> slow
    id 55/47/88/64/73  iso<=3 -> lock (55 iso=2 was instant)
So ISOLATED-cell count is the hardness predictor (raw transition count doesn't
separate 43 from the good id 42, which also has 16 transitions but iso=5).

SECDED(12,7) spans IDs 0-127, so there's headroom above the collectibles' 1-100,
and the old blacklist (11,59,82,91,94) was for ROTATION ambiguity that pinning
removes -- both are candidate pools if we need to dodge hard patterns.

Prints the hardness of the current collectible IDs and the size of the low-iso
pool across 1-127, so we can decide whether all 95 collectibles can map to
reliably-scannable patterns.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from secded import encode

CELL_ORDER = [(0,1),(0,2),(1,0),(1,1),(1,2),(1,3),(2,0),(2,1),(2,2),(2,3),(3,1),(3,2)]
BLACKLIST_1_100 = {11, 59, 82, 91, 94}     # were rotation-ambiguous; pinning may free them


def grid_for(idv):
    cw = encode(idv)
    g = [[0]*4 for _ in range(4)]
    g[0][0] = g[0][3] = g[3][0] = 1          # anchors (bump)
    g[3][3] = 0                              # guard (flat)
    for bit, (r, c) in zip(cw, CELL_ORDER):
        g[r][c] = bit
    return g


def isolated(g):
    n = 0
    for r in range(4):
        for c in range(4):
            nb = [(r+dr, c+dc) for dr, dc in ((-1,0),(1,0),(0,-1),(0,1))
                  if 0 <= r+dr < 4 and 0 <= c+dc < 4]
            if all(g[r][c] != g[a][b] for a, b in nb):
                n += 1
    return n


def transitions(g):
    t = 0
    for r in range(4):
        for c in range(4):
            if c < 3 and g[r][c] != g[r][c+1]: t += 1
            if r < 3 and g[r][c] != g[r+1][c]: t += 1
    return t


def hardness(idv):
    return isolated(grid_for(idv))


def main():
    ALL = range(1, 128)
    cur = [i for i in range(1, 101) if i not in BLACKLIST_1_100]   # current 95 collectible IDs
    def bucket(ids):
        from collections import Counter
        return dict(sorted(Counter(hardness(i) for i in ids).items()))
    print("isolated-cell histogram, current 95 collectible IDs:", bucket(cur))
    print("isolated-cell histogram, full codec 1-127:         ", bucket(ALL))
    for thr in (2, 3):
        pool = [i for i in ALL if hardness(i) <= thr]
        cur_ok = [i for i in cur if hardness(i) <= thr]
        print(f"\niso<={thr}: {len(pool)} IDs in 1-127 ({len(cur_ok)} of the current 95 already qualify)")
    hard_cur = sorted(i for i in cur if hardness(i) >= 6)
    print(f"\ncurrent collectible IDs with iso>=6 (43-class, likely FAIL): {hard_cur}")
    marg_cur = sorted(i for i in cur if 4 <= hardness(i) <= 5)
    print(f"current collectible IDs with iso 4-5 (marginal/slow): {len(marg_cur)} -> {marg_cur}")


if __name__ == "__main__":
    main()
