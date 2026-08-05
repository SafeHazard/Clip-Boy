"""Unit tests for the window-plurality-with-margin HR lock model (Phase A).

Validated on SYNTHETIC per-frame sequences (deterministic), not sweep_log 'saw'
aggregates (which are a low-rate poll sample -- see the plan's saw-sampling caveat).
Run: py -3 -m pytest scripts/hr_spec/test_vote_lock_model.py -v
  (or: py -3 scripts/hr_spec/test_vote_lock_model.py  -- has a __main__ runner)
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vote_lock_model import VoteLock, decide, WEIGHT_CLEAN, WEIGHT_CORR


def _run(seq, **kw):
    """seq = list of (id, clean, pose_ok); return the latched locked id or None."""
    vl = VoteLock(**kw)
    out = None
    for (i, cl, po) in seq:
        r = vl.feed(i, cl, po)
        if r is not None and out is None:
            out = r
    return out, vl


def test_true_id_dominant_locks_true_fast():
    # A correct tag reading corrected-consistent (syn!=0) with occasional clean.
    # 1 clean + 11 corrected of id 14 => weight 2 + 11 = 13 >= floor 12, no rival.
    seq = [(14, True, True)] + [(14, False, True)] * 11
    got, _ = _run(seq)
    assert got == 14


def test_minority_clean_neighbor_does_not_win():
    # THE headline case (shipped bug: id 14 saw 14x23;15x2 -> wrong-locked 15).
    # 23 corrected-14 vs 2 clean-15: 15's weighted votes (4) never clear the
    # 2x margin over 14 (>=23), and 14 dominates -> locks 14, never 15.
    seq = [(15, True, True), (15, True, True)] + [(14, False, True)] * 23
    got, vl = _run(seq)
    assert got == 14
    assert got != 15


def test_no_clear_winner_times_out():
    # id 18 real case: 54/127/118 with no >=2x winner -> never commits (safe timeout).
    seq = []
    for _ in range(10):
        seq += [(54, False, True), (127, False, True), (118, False, True)]
    got, vl = _run(seq)
    assert got is None
    (top, second) = vl.top2()
    assert top[1] * 10 < 20 * second[1]  # margin genuinely unmet


def test_early_neighbor_burst_ages_out_of_window():
    # A short clean-neighbor burst at the start must not poison a long correct read:
    # the sliding window (24) ages the burst out as correct frames accumulate.
    seq = [(96, True, True)] * 3 + [(45, False, True)] * 40
    got, _ = _run(seq, window=24)
    assert got == 45


def test_wrong_id_dominant_still_wrong_documents_phaseBC_boundary():
    # HONEST boundary: a hard PHYSICAL aliaser (e.g. old id 40 read as 72 dominantly)
    # is NOT fixable by lock policy -- the wrong id genuinely dominates the window.
    # Phase A locks the dominant id (72); these IDs are handled by the already-done
    # remap / Phase B/C, NOT here. This test PINS that expectation so nobody thinks
    # Phase A magically fixed physical aliasing.
    seq = [(72, True, True)] + [(72, False, True)] * 20
    got, _ = _run(seq)
    assert got == 72  # dominant wrong id; not a Phase-A regression, a Phase-B/C case


def test_corrected_only_correct_tag_locks():
    # THE core objective: a correct tag that reads CORRECTED every frame (syn!=0,
    # never a clean/exact codeword -- the common case at ~2 sensor-zones/cell) MUST
    # still lock. Clean-ness is a weight, not a gate; the dominant winner wins.
    seq = [(50, False, True)] * 30
    got, _ = _run(seq)
    assert got == 50


def test_pose_failed_frames_do_not_vote():
    # Frames that failed the pose gates (pose_ok=False) contribute nothing.
    seq = [(7, True, False)] * 30  # all pose-failed
    got, _ = _run(seq)
    assert got is None


def test_decide_stateless_matches():
    # decide() over a tallied dict: clear 2x winner over floor -> that id.
    assert decide({14: 25, 15: 4}, floor=12, margin_x10=20) == 14
    # margin unmet -> None.
    assert decide({14: 13, 15: 12}, floor=12, margin_x10=20) is None
    # evidence floor unmet -> None.
    assert decide({14: 6}, floor=12, margin_x10=20) is None
    # dominant winner, no runner-up, over floor -> that id (no clean needed).
    assert decide({7: 20}, floor=12, margin_x10=20) == 7


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
