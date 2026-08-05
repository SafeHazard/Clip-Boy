"""Aggregate a sweep_log.csv into the three headline first-scan-success numbers,
split by on-catalog vs free id. Uses the LATEST row per id.

Run: py -3 scripts/hr_spec/sweep_summary.py            # scores sweep_log.csv
     py -3 scripts/hr_spec/sweep_summary.py other.csv
"""
import csv, os, sys


def _catalog_ids(path):
    ids = set()
    with open(path, newline="", encoding="utf-8") as f:
        r = csv.reader(f); h = next(r); ic = h.index("ID")
        for row in r:
            if row and row[ic].strip().isdigit():
                ids.add(int(row[ic]))
    return ids


def summarize(rows, catalog_ids):
    latest = {}
    for row in rows:
        # A SKIP (operator no-op) is not an outcome: it must NOT overwrite a real
        # prior result, nor sit in the lock_rate denominator. Ignore non-outcome rows.
        if row.get("outcome") not in ("LOCK", "WRONG", "TIMEOUT"):
            continue
        latest[int(row["id"])] = row            # later real rows overwrite = latest wins
    buckets = {"on_catalog": [], "free": []}
    for idv, row in latest.items():
        buckets["on_catalog" if idv in catalog_ids else "free"].append(row)
    out = {}
    for k, rs in buckets.items():
        n = len(rs)
        lock = sum(1 for r in rs if r["outcome"] == "LOCK")
        wrong = sum(1 for r in rs if r["outcome"] == "WRONG")
        timeout = sum(1 for r in rs if r["outcome"] == "TIMEOUT")
        ttls = [float(r["time_to_lock_s"]) for r in rs
                if r["outcome"] == "LOCK" and str(r.get("time_to_lock_s", "")).strip()]
        out[k] = {
            "n": n, "lock": lock, "wrong": wrong, "timeout": timeout,
            "lock_rate": (lock / n) if n else 0.0,
            "mean_ttl": (sum(ttls) / len(ttls)) if ttls else 0.0,
        }
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    logp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "sweep_log.csv")
    catp = os.path.join(here, "..", "..", "data", "collectibles.csv")
    with open(logp, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    s = summarize(rows, _catalog_ids(catp))
    for k in ("on_catalog", "free"):
        d = s[k]
        print(f"{k:11s} n={d['n']:3d}  lock={d['lock']:3d} ({d['lock_rate']*100:5.1f}%)  "
              f"wrong={d['wrong']:3d}  timeout={d['timeout']:3d}  mean_ttl={d['mean_ttl']:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
