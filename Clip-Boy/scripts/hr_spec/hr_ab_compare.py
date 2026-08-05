#!/usr/bin/env python3
"""Side-by-side A/B report for the HR lock-policy sweep (vote-lock vs legacy).

Feed it the two sweep logs produced by running sweep_harness.py under each policy:
  # 1. calibrate the badge first (aim at a flat matte surface): serial `hr_calibrate`
  # 2. vote-lock pass:
  CB_LOCKPOLICY=1 py -3 scripts/hr_spec/sweep_harness.py all
  mv scripts/hr_spec/sweep_log.csv scripts/hr_spec/sweep_votelock.csv
  # 3. legacy pass (same tags):
  CB_LOCKPOLICY=0 py -3 scripts/hr_spec/sweep_harness.py all
  mv scripts/hr_spec/sweep_log.csv scripts/hr_spec/sweep_legacy.csv
  # 4. compare:
  py -3 scripts/hr_spec/hr_ab_compare.py scripts/hr_spec/sweep_votelock.csv scripts/hr_spec/sweep_legacy.csv

Reports on-catalog lock-rate / mean-ttl / wrong / timeout for each policy, the deltas,
and per-id outcome changes (esp. any NEW wrong-lock under vote-lock, which is the one
thing that must stay at zero on the shipping catalog).
"""
import csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sweep_summary import summarize, _catalog_ids


def _load(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def _latest(rows):
    out = {}
    for r in rows:
        if r.get("outcome") in ("LOCK", "WRONG", "TIMEOUT"):
            out[int(r["id"])] = r
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    vote_p, legacy_p = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    cat = _catalog_ids(os.path.join(here, "..", "..", "data", "collectibles.csv"))
    vote, legacy = _load(vote_p), _load(legacy_p)
    sv, sl = summarize(vote, cat), summarize(legacy, cat)

    print("=" * 68)
    print("HR LOCK-POLICY A/B  (on-catalog)")
    print("=" * 68)
    print(f"{'metric':16s} {'VOTE-LOCK':>12s} {'LEGACY':>12s} {'delta':>12s}")
    v, l = sv["on_catalog"], sl["on_catalog"]
    print(f"{'n':16s} {v['n']:12d} {l['n']:12d} {'':>12s}")
    print(f"{'lock':16s} {v['lock']:12d} {l['lock']:12d} {v['lock']-l['lock']:+12d}")
    print(f"{'lock_rate %':16s} {v['lock_rate']*100:12.1f} {l['lock_rate']*100:12.1f} {(v['lock_rate']-l['lock_rate'])*100:+12.1f}")
    print(f"{'wrong':16s} {v['wrong']:12d} {l['wrong']:12d} {v['wrong']-l['wrong']:+12d}")
    print(f"{'timeout':16s} {v['timeout']:12d} {l['timeout']:12d} {v['timeout']-l['timeout']:+12d}")
    print(f"{'mean_ttl s':16s} {v['mean_ttl']:12.1f} {l['mean_ttl']:12.1f} {v['mean_ttl']-l['mean_ttl']:+12.1f}")

    # Per-id outcome changes (on-catalog).
    lv, ll = _latest(vote), _latest(legacy)
    changed = []
    new_wrong = []
    for idv in sorted(set(lv) | set(ll)):
        if idv not in cat:
            continue
        ov = lv.get(idv, {}).get("outcome", "-")
        ol = ll.get(idv, {}).get("outcome", "-")
        if ov != ol:
            changed.append((idv, ol, ov, lv.get(idv, {}).get("locked_id", "")))
            if ov == "WRONG":
                new_wrong.append(idv)
    print("\nper-id outcome changes (legacy -> vote-lock):")
    if not changed:
        print("  (none)")
    for idv, ol, ov, got in changed:
        flag = "   <-- NEW WRONG-LOCK" if ov == "WRONG" else ""
        print(f"  id {idv:3d}: {ol:8s} -> {ov:8s}" + (f" (got {got})" if ov == "WRONG" else "") + flag)

    print("\n" + "=" * 68)
    if new_wrong:
        print(f"!!! {len(new_wrong)} NEW on-catalog wrong-lock(s) under vote-lock: {new_wrong}")
        print("    -> raise kVoteMarginX10 (HRScanEngine.h) and re-run before merge.")
    else:
        print("OK: zero new on-catalog wrong-locks under vote-lock.")
    print("=" * 68)
    return 0


if __name__ == "__main__":
    sys.exit(main())
