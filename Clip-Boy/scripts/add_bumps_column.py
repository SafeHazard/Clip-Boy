"""Add a 'Bumps' column to data/collectibles.csv computed from each ID.

Total bump count for an HR code = 3 (orient cells) + popcount(ID) + popcount(CRC4(ID)).
Bumps determines scan reliability: ID 128 (5 bumps) scans easily, ID 55 (9 bumps)
is borderline. Putting this in the CSV lets us pair high-rarity collectibles
(Legendary, Mythic) with high-bump (harder-to-scan) IDs — rare cards stay rare.

Usage:
  py -3 scripts/add_bumps_column.py        # adds/updates Bumps column in place
  py -3 scripts/add_bumps_column.py --dry  # show what would change

Idempotent: rerunning recomputes bumps and overwrites the existing column.
Run scripts/embed_csv.py afterward to refresh the PROGMEM version.
"""
import argparse
import csv
import os

INPUT = os.path.join(os.path.dirname(__file__), "..", "data", "collectibles.csv")


def crc4(id8: int) -> int:
    return (id8 ^ (id8 >> 4)) & 0xF


def total_bumps(id8: int) -> int:
    return 3 + bin(id8).count("1") + bin(crc4(id8)).count("1")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry", action="store_true",
                    help="Print the proposed change without writing the file.")
    args = ap.parse_args()

    with open(INPUT, "r", encoding="utf-8", newline="") as f:
        reader = csv.reader(f)
        rows = list(reader)

    if not rows:
        print("CSV is empty.")
        return

    header = rows[0]
    body = rows[1:]

    # Add or overwrite trailing 'Bumps' column.
    if header[-1].strip().lower() == "bumps":
        # Already present — overwrite values
        for r in body:
            if not r:
                continue
            try:
                r[-1] = str(total_bumps(int(r[0])))
            except ValueError:
                pass
        msg = f"Updated existing Bumps column ({len(body)} rows)."
    else:
        header.append("Bumps")
        for r in body:
            if not r:
                continue
            try:
                r.append(str(total_bumps(int(r[0]))))
            except ValueError:
                r.append("")
        msg = f"Added Bumps column ({len(body)} rows)."

    if args.dry:
        # Show first 3 rows + last 3 rows for sanity check
        import sys
        out = csv.writer(sys.stdout)
        out.writerow(header)
        for r in body[:3]:
            out.writerow(r)
        print("...")
        for r in body[-3:]:
            out.writerow(r)
        # Histogram
        from collections import Counter
        bump_counts = Counter()
        for r in body:
            try:
                bump_counts[int(r[-1])] += 1
            except (ValueError, IndexError):
                pass
        print()
        print("Bump-count distribution:")
        for k in sorted(bump_counts):
            print(f"  {k} bumps: {bump_counts[k]} collectibles")
        print(f"\n[dry run] {msg}")
        return

    with open(INPUT, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        for r in body:
            writer.writerow(r)
    print(f"OK: {msg} -> {INPUT}")
    print("Now run: py -3 scripts/embed_csv.py")


if __name__ == "__main__":
    main()
