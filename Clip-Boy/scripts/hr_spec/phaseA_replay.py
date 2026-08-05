"""Phase A vote-lock replay against the legacy sweep_log.csv.

Directional, static replay: for each tag id, take its LATEST non-SKIP sweep
row, parse the low-rate `saw` histogram into a vote dict, and ask
vote_lock_model.decide() what the vote-lock policy WOULD have done, using
the same distribution the legacy gate saw. Compare against the row's actual
legacy outcome (LOCK/WRONG/TIMEOUT).

*** HONEST CAVEAT (read this before trusting any number below) ***
`saw` is a LOW-RATE POLL SAMPLE of the decode stream during that scan, not
the true per-frame distribution -- e.g. one row locked id 19 while `saw`
only shows 127x11;54x8;55x6 (19 isn't even in the top-3 sample). It also
carries NO per-frame syn (clean vs corrected) info, so this replay treats
every seen count as weight 1 (the "conservative, all-corrected" reading --
clean frames would only make the real vote-lock MORE decisive, never less).
This makes the analysis DIRECTIONAL ONLY. It is not a prediction of what a
recording vote-lock firmware will actually do frame-by-frame, and it is NOT
a substitute for the calibrated Task-4 A/B, which is the definitive number.

Usage: py -3 scripts/hr_spec/phaseA_replay.py
"""
import csv
import os
import re
import sys
from collections import defaultdict, Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vote_lock_model import decide

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SWEEP_CSV = os.path.join(ROOT, "scripts", "hr_spec", "sweep_log.csv")
CATALOG_CSV = os.path.join(ROOT, "data", "collectibles.csv")

SAW_TOKEN_RE = re.compile(r"^(\d+)x(\d+)$")


def load_catalog_ids(path=CATALOG_CSV):
    ids = set()
    with open(path, encoding="utf-8") as f:
        r = csv.reader(f)
        next(r)  # header
        for row in r:
            if row and row[0].strip():
                ids.add(int(row[0]))
    return ids


def parse_saw(saw_str):
    """'88x11;73x24;72x6' -> {88:11, 73:24, 72:6}. Weight = 1 per seen count
    (conservative -- we lack clean/corrected syn info from the low-rate poll)."""
    votes = {}
    if not saw_str:
        return votes
    for tok in saw_str.split(";"):
        tok = tok.strip()
        if not tok:
            continue
        m = SAW_TOKEN_RE.match(tok)
        if not m:
            continue  # malformed token, skip defensively
        tid, cnt = int(m.group(1)), int(m.group(2))
        votes[tid] = votes.get(tid, 0) + cnt
    return votes


def load_latest_rows(path=SWEEP_CSV):
    """Latest non-SKIP row per id, keyed by id (int). File-order = time-order,
    so the last occurrence in the file wins."""
    latest = {}
    with open(path, encoding="utf-8") as f:
        r = csv.reader(f)
        header = next(r)
        idx = {name: i for i, name in enumerate(header)}
        for row in r:
            if not row:
                continue
            if row[idx["outcome"]] == "SKIP":
                continue
            tid = int(row[idx["id"]])
            latest[tid] = row
        return latest, idx


def classify_predicted(votes, true_id, floor=12, margin_x10=20):
    winner = decide(votes, floor=floor, margin_x10=margin_x10)
    if winner is None:
        return "NO-LOCK"
    if winner == true_id:
        return "LOCK-correct"
    return "LOCK-wrong"


def main():
    catalog_ids = load_catalog_ids()
    latest, idx = load_latest_rows()

    # transitions[on_catalog][legacy_outcome][predicted_class] = count
    transitions = defaultdict(lambda: defaultdict(Counter))
    regressions = []  # (id, name, legacy_outcome, predicted, votes)
    detail_rows = []

    for tid, row in sorted(latest.items()):
        name = row[idx["name"]]
        legacy_outcome = row[idx["outcome"]]
        saw = row[idx["saw"]]
        votes = parse_saw(saw)
        predicted = classify_predicted(votes, tid)
        on_cat = tid in catalog_ids
        bucket = "on-catalog" if on_cat else "free/off-catalog"

        transitions[bucket][legacy_outcome][predicted] += 1
        detail_rows.append((tid, name, legacy_outcome, predicted, saw, on_cat))

        if on_cat and legacy_outcome == "LOCK" and predicted == "LOCK-wrong":
            regressions.append((tid, name, legacy_outcome, predicted, saw))

    print("=" * 78)
    print("PHASE A VOTE-LOCK REPLAY (default floor=12, margin_x10=20)")
    print("=" * 78)
    print(
        "CAVEAT: 'saw' is a low-rate poll sample with NO clean/corrected info.\n"
        "This is a DIRECTIONAL replay of observed distributions, NOT a\n"
        "per-frame prediction. The calibrated Task-4 A/B is the definitive\n"
        "number. See module docstring for the full caveat.\n"
    )

    for bucket in ("on-catalog", "free/off-catalog"):
        n_ids = sum(1 for r in detail_rows if r[5] == (bucket == "on-catalog"))
        print(f"--- {bucket} ({n_ids} ids) ---")
        for legacy in ("WRONG", "TIMEOUT", "LOCK"):
            row_counts = transitions[bucket].get(legacy)
            if not row_counts:
                continue
            total = sum(row_counts.values())
            parts = ", ".join(f"{k}={v}" for k, v in sorted(row_counts.items()))
            print(f"  legacy {legacy:8s} (n={total:3d}): {parts}")
        print()

    if regressions:
        print("!" * 78)
        print(f"!! REGRESSION ALERT: {len(regressions)} on-catalog id(s) went legacy-LOCK-correct -> predicted-LOCK-WRONG !!")
        for tid, name, legacy_outcome, predicted, saw in regressions:
            print(f"   id={tid:3d} {name!r}: legacy={legacy_outcome} predicted={predicted} saw={saw!r}")
        print("!" * 78)
    else:
        print("No on-catalog regressions (legacy-LOCK -> predicted-LOCK-wrong) at default knobs.")

    print()
    print("=" * 78)
    print("PER-ID DETAIL")
    print("=" * 78)
    print(f"{'id':>4} {'cat':>4} {'legacy':8} {'predicted':12} name / saw")
    for tid, name, legacy_outcome, predicted, saw, on_cat in detail_rows:
        cat_mark = "Y" if on_cat else "n"
        print(f"{tid:4d} {cat_mark:>4} {legacy_outcome:8} {predicted:12} {name} [{saw}]")


if __name__ == "__main__":
    main()
