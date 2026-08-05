#!/usr/bin/env python3
"""oracle_merge.py -- combine per-STAGE oracle manifests into one final manifest + gate verdict.

The full oracle suite can wedge the recycled boards' native USB-CDC if run in one shot (too many
serial reconnects over a long run). Splitting it into STAGES with a physical un/replug between each
(run_oracle_suite.py --tests <subset> --out stageN.json) sidesteps that: every stage starts on a
fresh CDC, so no single stage accumulates enough reconnects to wedge. This merges the stage
manifests, CHECKS that every registered test is covered exactly once (a forgotten stage cannot pass
as green), and applies the SAME gate policy as run_oracle_suite.gate_verdict:
  T0 strict (any non-PASS hard-fails) ; T1/T2 FAIL hard-fails ; an honest CANNOT-TEST is owner-ackable.

Usage: py -3 oracle_merge.py stageA.json stageB.json ... [--out oracle-manifest.json]
rc: 0=PASS, 1=FAIL, 2=INCOMPLETE (a test never ran), 3=NEEDS-ACK.
"""
import argparse, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_oracle_suite import REGISTRY, PASS, FAIL, CANT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stages", nargs="+", help="per-stage manifest JSON files")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "docs", "test-reports",
                    "oracle-manifest.json"))
    a = ap.parse_args()

    expected = {t.name for t in REGISTRY}
    rows_by_name = {}          # name -> row ; a name appearing in two stages is a DUP (flagged)
    dupes, fleet = [], {}
    witness_any = False
    for f in a.stages:
        m = json.load(open(f, encoding="utf-8"))
        fleet.update(m.get("fleet", {}))
        witness_any = witness_any or bool(m.get("witness_ready"))
        for r in m.get("rows", []):
            nm = r.get("name")
            if nm in rows_by_name:
                dupes.append(nm)
            rows_by_name[nm] = r

    covered = set(rows_by_name)
    missing = sorted(expected - covered)
    extra = sorted(covered - expected)
    rows = [rows_by_name[n] for n in sorted(rows_by_name)]
    summary = {v: sum(1 for r in rows if r.get("verdict") == v) for v in (PASS, FAIL, CANT)}

    # Gate policy -- mirror run_oracle_suite.gate_verdict exactly.
    hard_fail = needs_ack = False
    for r in rows:
        v = r.get("verdict")
        if r.get("tier") == "T0":
            if v != PASS:
                hard_fail = True
        elif v == FAIL:
            hard_fail = True
        elif v == CANT:
            needs_ack = True

    manifest = dict(fleet=fleet, witness_ready=witness_any, staged=True,
                    stage_files=[os.path.basename(f) for f in a.stages], rows=rows, summary=summary,
                    coverage=dict(expected=len(expected), covered=len(covered),
                                  missing=missing, duplicated=sorted(set(dupes))))
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)

    print("[merge] %d stage(s) -> %d/%d registered tests covered" % (len(a.stages), len(covered), len(expected)))
    print("  summary: %s" % summary)
    print("  -> %s" % a.out)
    if extra:
        print("  WARN unknown rows (not in REGISTRY, ignored for coverage): %s" % ", ".join(extra))
    if dupes:
        print("  WARN a test appeared in >1 stage (kept the last): %s" % ", ".join(sorted(set(dupes))))
    if missing:
        print("  MISSING (never ran in any stage): %s" % ", ".join(missing))
        print("\nGATE: INCOMPLETE -- %d test(s) never ran. NOT a pass; run the remaining stage(s)." % len(missing))
        return 2
    if hard_fail:
        print("\nGATE: FAIL (a T0 gate did not PASS, or a T1/T2 oracle FAILed)")
        return 1
    if needs_ack:
        print("\nGATE: NEEDS OWNER ACK -- every real test PASSed; some were CANNOT-TEST (a fixture "
              "was absent). Review the manifest's CANNOT-TEST rows and ack before release.")
        return 3
    print("\nGATE: PASS -- all tiers green.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
