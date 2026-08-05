#!/usr/bin/env python3
"""Remap the 95 collectibles onto the most reliably-scannable HR IDs.

After the anchor-scanner rewrite (graded rings + pinned guard), a tag's scan
reliability is predicted by scripts/hr_spec/reliability_sim.py (a Monte-Carlo
model of the decode pipeline, calibrated to hardware). This script reassigns the
collectibles' ID column in data/collectibles.csv so every item lands on a
reliable ID, while MINIMIZING reprints (an item keeps its current ID if that ID
is already reliable).

Input: a ranked reliability file (one `id score` per line, best first, reliable
only) -- generate it from reliability_sim.py. Pass its path as arg 1
(default: scripts/hr_spec/reliable_ids.txt).

Output: rewrites data/collectibles.csv (ID column only) and writes a remap report
data/remap_report.csv (old_id,new_id,title) listing exactly which physical tags
must be reprinted. Content/columns are otherwise untouched.

Run scripts/embed_csv.py (or scripts/build.sh) afterward to refresh PROGMEM.
Idempotent-ish: re-running with the same ranking is a no-op once all IDs are reliable.
"""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(HERE, "..", "data", "collectibles.csv")
REPORT_PATH = os.path.join(HERE, "..", "data", "remap_report.csv")
DEFAULT_RANK = os.path.join(HERE, "hr_spec", "reliable_ids.txt")


def load_ranking(path):
    """Return [ids] best-first, reliable only."""
    ids = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            ids.append(int(line.split()[0]))
    if len(set(ids)) != len(ids):
        raise SystemExit("ranking file has duplicate IDs")
    return ids


def main():
    rank_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_RANK
    ranked = load_ranking(rank_path)
    reliable = set(ranked)

    with open(CSV_PATH, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    header, data = rows[0], rows[1:]
    id_col = header.index("ID")

    n = len(data)
    if len(ranked) < n:
        raise SystemExit(f"only {len(ranked)} reliable IDs for {n} collectibles -- widen the pool")

    cur_ids = [int(r[id_col]) for r in data]
    kept = {i for i in cur_ids if i in reliable}
    # Fresh reliable IDs to hand out, best-ranked first, not already kept.
    pool = [i for i in ranked if i not in kept]

    remap = []  # (row_index, old, new, title)
    pi = 0
    for ri, r in enumerate(data):
        old = int(r[id_col])
        if old in reliable:
            continue                      # keep -> no reprint
        new = pool[pi]; pi += 1
        r[id_col] = str(new)
        remap.append((ri, old, new, r[header.index("Title")]))

    # sanity: unique IDs, all in-range and reliable
    new_ids = [int(r[id_col]) for r in data]
    assert len(set(new_ids)) == n, "ID collision after remap!"
    assert all(i in reliable and 1 <= i <= 127 for i in new_ids), "a final ID is unreliable/out-of-range!"

    with open(CSV_PATH, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(data)

    with open(REPORT_PATH, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["old_id", "new_id", "title"])
        for _, old, new, title in remap:
            w.writerow([old, new, title])

    print(f"remapped {len(remap)} of {n} collectibles onto reliable IDs "
          f"({n - len(remap)} kept their ID -> no reprint).")
    print(f"reprint list: {os.path.relpath(REPORT_PATH)}")
    if remap:
        print("  " + ", ".join(f"{o}->{nw}" for _, o, nw, _ in remap[:20]) +
              (" ..." if len(remap) > 20 else ""))


if __name__ == "__main__":
    main()
