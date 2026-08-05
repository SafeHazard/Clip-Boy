"""Phase A vote-lock knob sensitivity sweep (sibling to phaseA_replay.py).

Sweeps floor x margin_x10 over the SAME static replay data (each id's latest
non-SKIP sweep_log.csv row, saw-histogram parsed to a weight-1 vote dict) and
reports, per combo, the on-catalog predicted lock-correct / predicted-wrong /
predicted-timeout counts. Same directional/static caveats as phaseA_replay.py
apply in full -- see that module's docstring. This sweep answers "which knob
values are relatively safer or riskier", not "what will the real lock rate be".

Usage: py -3 scripts/hr_spec/phaseA_knob_sweep.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phaseA_replay import load_latest_rows, load_catalog_ids, parse_saw
from vote_lock_model import decide

FLOORS = [8, 10, 12, 14, 16]
MARGINS = [15, 20, 25, 30]
DEFAULT_FLOOR = 12
DEFAULT_MARGIN = 20


def sweep():
    catalog_ids = load_catalog_ids()
    latest, idx = load_latest_rows()

    # Precompute (true_id, votes) once for on-catalog ids only.
    on_catalog_cases = []
    for tid, row in latest.items():
        if tid not in catalog_ids:
            continue
        votes = parse_saw(row[idx["saw"]])
        on_catalog_cases.append((tid, votes))

    results = {}
    for floor in FLOORS:
        for margin in MARGINS:
            correct = wrong = timeout = 0
            for tid, votes in on_catalog_cases:
                winner = decide(votes, floor=floor, margin_x10=margin)
                if winner is None:
                    timeout += 1
                elif winner == tid:
                    correct += 1
                else:
                    wrong += 1
            results[(floor, margin)] = (correct, wrong, timeout)
    return results, len(on_catalog_cases)


def main():
    results, n = sweep()
    print("=" * 78)
    print(f"PHASE A KNOB SWEEP -- on-catalog ids, n={n}")
    print("(same static-replay caveats as phaseA_replay.py -- directional only)")
    print("=" * 78)
    header = f"{'floor':>6} {'margin_x10':>10} {'lock-correct':>13} {'pred-wrong':>11} {'pred-timeout':>13}"
    print(header)
    for floor in FLOORS:
        for margin in MARGINS:
            correct, wrong, timeout = results[(floor, margin)]
            mark = " <- current default" if (floor, margin) == (DEFAULT_FLOOR, DEFAULT_MARGIN) else ""
            print(f"{floor:6d} {margin:10d} {correct:13d} {wrong:11d} {timeout:13d}{mark}")
        print()

    # Knee-finding: among combos with pred-wrong == the observed minimum,
    # pick the one maximizing lock-correct. Ties broken toward the combo
    # closest to the current default (12, 20) by absolute knob distance.
    min_wrong = min(w for (_c, w, _t) in results.values())
    zero_or_min_wrong = {k: v for k, v in results.items() if v[1] == min_wrong}
    best = max(
        zero_or_min_wrong.items(),
        key=lambda kv: (kv[1][0], -(abs(kv[0][0] - DEFAULT_FLOOR) + abs(kv[0][1] - DEFAULT_MARGIN))),
    )
    (bfloor, bmargin), (bcorrect, bwrong, btimeout) = best
    print("-" * 78)
    print(f"Minimum observed pred-wrong across the grid: {min_wrong}")
    print(
        f"Knee candidate: floor={bfloor}, margin_x10={bmargin} -> "
        f"lock-correct={bcorrect}, pred-wrong={bwrong}, pred-timeout={btimeout}"
    )
    default_correct, default_wrong, default_timeout = results[(DEFAULT_FLOOR, DEFAULT_MARGIN)]
    print(
        f"Current firmware default floor={DEFAULT_FLOOR}, margin_x10={DEFAULT_MARGIN} -> "
        f"lock-correct={default_correct}, pred-wrong={default_wrong}, pred-timeout={default_timeout}"
    )
    print("-" * 78)


if __name__ == "__main__":
    main()
