import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sweep_summary import summarize

def test_summarize_splits_and_rates():
    # rows: (id, outcome, locked_id, ttl)  -- minimal shape summarize() needs.
    rows = [
        {"id": 15, "outcome": "LOCK",    "locked_id": 15, "time_to_lock_s": "1.5"},
        {"id": 15, "outcome": "LOCK",    "locked_id": 15, "time_to_lock_s": "1.1"},  # latest wins
        {"id": 40, "outcome": "WRONG",   "locked_id": 72, "time_to_lock_s": "6.0"},
        {"id": 3,  "outcome": "TIMEOUT", "locked_id": -1, "time_to_lock_s": ""},
        {"id": 200,"outcome": "LOCK",    "locked_id": 200,"time_to_lock_s": "2.0"},  # not in catalog
    ]
    catalog = {15, 40, 3}
    s = summarize(rows, catalog)
    # on-catalog: latest per id -> 15 LOCK(1.1), 40 WRONG, 3 TIMEOUT
    assert s["on_catalog"]["n"] == 3
    assert s["on_catalog"]["lock"] == 1
    assert s["on_catalog"]["wrong"] == 1
    assert s["on_catalog"]["timeout"] == 1
    assert abs(s["on_catalog"]["lock_rate"] - (1/3)) < 1e-6
    assert abs(s["on_catalog"]["mean_ttl"] - 1.1) < 1e-6   # only the locked row counts
    assert s["free"]["n"] == 1 and s["free"]["lock"] == 1


def test_skip_rows_excluded():
    # An operator SKIP must not erase a real prior outcome nor dilute lock_rate.
    rows = [
        {"id": 15, "outcome": "LOCK", "locked_id": 15, "time_to_lock_s": "1.5"},
        {"id": 15, "outcome": "SKIP", "locked_id": "", "time_to_lock_s": ""},   # later, but a no-op
        {"id": 40, "outcome": "SKIP", "locked_id": "", "time_to_lock_s": ""},   # only-SKIP id -> excluded
    ]
    s = summarize(rows, {15, 40})
    assert s["on_catalog"]["n"] == 1              # only id 15 counts (40 was skip-only)
    assert s["on_catalog"]["lock"] == 1
    assert abs(s["on_catalog"]["lock_rate"] - 1.0) < 1e-6


if __name__ == "__main__":
    import traceback
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    fails = 0
    for fn in fns:
        try:
            fn(); print(f"PASS {fn.__name__}")
        except Exception:
            fails += 1; print(f"FAIL {fn.__name__}"); traceback.print_exc()
    print(f"\n{len(fns)-fails}/{len(fns)} passed")
    sys.exit(1 if fails else 0)
