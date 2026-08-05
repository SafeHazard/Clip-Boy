#!/usr/bin/env python3
"""DC34-155 lock-robustness simulator.

Reproduces the hand-held wrong-lock failure (badge locks id-3 / id-72 instead
of id-16 when moving) in software, so gate/lock hardenings can be compared
without hardware. Bit-exact decode mirrors HRScanEngine.cpp.

Two decoders:
  - decode3: current 3-corner gate (g00 & g03 & g30)
  - decode4: hardened 4-corner gate (+ !g33, guard must read FAR)
Returns (status, id, rotation, syn) so lock rules keyed on cleanliness (syn==0)
can be evaluated.
"""
import sys, random
sys.path.insert(0, r'C:\Users\data\OneDrive\esp\libraries\HRCode4x4\examples')
import hm_codegen as H

DR = [0,0,1,1,1,1,2,2,2,2,3,3]
DC = [1,2,0,1,2,3,0,1,2,3,1,2]
PPOS = [1,2,4,8]
DPOS = [3,5,6,7,9,10,11]


def rot90cw(g):
    return [[g[3-c][r] for c in range(4)] for r in range(4)]


def _secded(g):
    """Return (good, id, syn, c) for a canonical-oriented grid g."""
    h = [0]*12
    for i in range(11):
        h[i+1] = g[DR[i]][DC[i]]
    overall = g[DR[11]][DC[11]]
    syn = 0
    for p in PPOS:
        s = 0
        for j in range(1,12):
            if j & p: s ^= h[j]
        if s: syn |= p
    c = overall
    for j in range(1,12): c ^= h[j]
    good = False
    if syn == 0 and c == 0:
        good = True
    elif c == 1:
        if 1 <= syn <= 11: h[syn] ^= 1
        good = True
    rid = 0
    for i in range(7): rid |= h[DPOS[i]] << i
    return good, rid, syn, c


def decode(bits16, four_corner):
    base = [[bits16[r*4+c] & 1 for c in range(4)] for r in range(4)]
    status = 'NoOrient'; rid = 0; rot = 0; rsyn = -1
    for t in range(4):
        g = [row[:] for row in base]
        for _ in range(t): g = rot90cw(g)
        gate = g[0][0] and g[0][3] and g[3][0]
        if four_corner:
            gate = gate and (not g[3][3])
        if not gate:
            continue
        if status == 'NoOrient':
            status = 'BadCRC'
        good, gid, syn, c = _secded(g)
        rid, rot, rsyn = gid, t, syn
        if good:
            return ('Ok', gid, t, syn)
    return (status, rid, rot, rsyn)


def grid_to_bits16(g):
    return [g[r][c] for r in range(4) for c in range(4)]


# ---------------------------------------------------------------------------
# Phase 1: characterize the garbage. For tag-16, enumerate physical rotations
# + guard-misread + up to 2 cell flips, and see which ids decode Ok and whether
# they are CLEAN (syn==0) or CORRECTED (syn!=0).
# ---------------------------------------------------------------------------
def characterize(target=16, four_corner=False):
    C = H.build_anchor_grid(target)
    cells12 = [(DR[k], DC[k]) for k in range(12)]
    from collections import Counter
    id_clean = Counter()      # id -> count of CLEAN (syn0) Ok reads
    id_corr = Counter()       # id -> count of CORRECTED Ok reads
    total = 0
    for pr in range(4):                        # physical rotation
        pg = C
        for _ in range(pr): pg = rot90cw(pg)
        for g33 in (0, 1):                     # guard misread as near?
            base = [row[:] for row in pg]
            base[3][3] = g33 if pr == 0 else base[3][3]  # only meaningful pre-rot; approximate
            # enumerate 0,1,2 cell flips on the 12 data cells
            import itertools
            for k in range(0, 3):
                for combo in itertools.combinations(range(12), k):
                    gg = [row[:] for row in base]
                    for idx in combo:
                        r,c = cells12[idx]; gg[r][c] ^= 1
                    st, rid, rot, syn = decode(grid_to_bits16(gg), four_corner)
                    total += 1
                    if st == 'Ok':
                        if syn == 0: id_clean[rid] += 1
                        else: id_corr[rid] += 1
    return id_clean, id_corr, total


# ---------------------------------------------------------------------------
# Phase 2: hand-held frame-sequence sim + full lock FSM. Model a scan as ~N
# frames while the hand drifts the tag's rotation and the sensor adds cell
# noise + guard misreads. Compare gate/lock-rule variants on P(correct lock)
# / P(WRONG lock) / P(no lock).
# ---------------------------------------------------------------------------
LOCK_REQUIRED = 12
DECAY = 3
CLEAR_BAD = 8


def run_fsm(frames, four_corner, clean_lock):
    """frames: list of bits16. Returns ('correct'|'wrong'|'none', locked_id)."""
    cand = -1; run = 0; locked = -1; bad = 0
    for bits in frames:
        st, rid, rot, syn = decode(bits, four_corner)
        ok = (st == 'Ok')
        advance = ok and (syn == 0 if clean_lock else True)
        if advance:
            bad = 0
            if rid == cand: run += 1
            else: cand = rid; run = 1
            if run >= LOCK_REQUIRED:
                return ('correct' if rid == 16 else 'wrong', rid)
        elif ok and clean_lock and rid == cand:
            # corrected read of the same candidate: neutral hold (don't advance/reset)
            pass
        else:
            if run > DECAY: run -= DECAY
            else: run = 0; cand = -1
            bad += 1
            if bad >= CLEAR_BAD: locked = -1
    return ('none', -1)


def make_frames(rng, n, p_flip, p_guard, bad_pose_frac):
    """Simulate a hand-held scan of tag-16 as a sequence of PERSISTENT poses.

    A pose dwells for several frames (the hand holds ~roughly still, then
    shifts). A 'bad' pose has a systematic corruption held for its whole dwell:
    an effective 90-deg rotation offset (affine snapped wrong) + the guard
    biased to misread near + possibly a dropped anchor -> a CONSISTENT wrong-id
    read across the dwell (this is what lets a wrong id reach 12-in-a-row and
    lock). A 'good' pose reads canonical id-16 with light per-cell flicker.
    """
    C = H.build_anchor_grid(16)
    cells = [(r, c) for r in range(4) for c in range(4)]
    frames = []
    i = 0
    while i < n:
        dwell = rng.randint(6, 20)               # frames this pose lasts
        bad = rng.random() < bad_pose_frac
        rot_off = rng.choice((1, 2, 3)) if bad else 0
        guard_bias = 0.9 if bad else p_guard     # bad pose consistently trips the guard
        drop_anchor = bad and rng.random() < 0.5 # systematic anchor dropout in a bad pose
        drop_idx = rng.choice((0, 3, 12)) if drop_anchor else None  # (0,0)/(0,3)/(3,0)
        for _ in range(min(dwell, n - i)):
            g = C
            for _ in range((0 + rot_off) % 4): g = rot90cw(g)
            gg = [row[:] for row in g]
            for (r, c) in cells:
                if rng.random() < p_flip: gg[r][c] ^= 1
            if rng.random() < guard_bias: gg[3][3] = 1
            if drop_idx is not None:
                r, c = cells[drop_idx]; gg[r][c] = 0
            frames.append(grid_to_bits16(gg))
        i += dwell
    return frames[:n]


def phase2(trials=4000):
    print("\n=== Phase 2: hand-held scan sim (full lock FSM) ===")
    # noise tuned so the CURRENT config wrong-locks a lot (matches ~2/3 hand-held)
    p_flip, p_guard, bad_pose_frac, N = 0.05, 0.10, 0.5, 150
    variants = [
        ("current: 3-corner, advance-any", False, False),
        ("Fix A: 4-corner, advance-any",   True,  False),
        ("Fix B: 3-corner + clean-lock",   False, True),
        ("Fix C: 4-corner + clean-lock",   True,  True),
    ]
    print(f"  noise: p_flip={p_flip} p_guard={p_guard} bad_pose_frac={bad_pose_frac} "
          f"frames={N} trials={trials}")
    for name, fc, cl in variants:
        c = w = n = 0
        rng = random.Random(1234)               # same noise stream across variants
        for _ in range(trials):
            frames = make_frames(rng, N, p_flip, p_guard, bad_pose_frac)
            res, _ = run_fsm(frames, fc, cl)
            if res == 'correct': c += 1
            elif res == 'wrong': w += 1
            else: n += 1
        print(f"  {name:34s}  correct={c/trials:5.1%}  WRONG={w/trials:5.1%}  none={n/trials:5.1%}")


def exhaustive_min_flips(target=16, four_corner=False, maxk=4):
    """Over all 4 physical rotations, find the MINIMUM number of flipped cells
    (any of the 16, incl. anchors + guard) that makes the decoder return Ok on
    a WRONG id. Returns (min_flips_or_None, sample)."""
    import itertools
    C = H.build_anchor_grid(target)
    all16 = [(r, c) for r in range(4) for c in range(4)]
    best = None; sample = None
    for pr in range(4):
        pg = C
        for _ in range(pr): pg = rot90cw(pg)
        for k in range(0, maxk + 1):
            if best is not None and k >= best:
                break
            found_k = False
            for combo in itertools.combinations(range(16), k):
                gg = [row[:] for row in pg]
                for idx in combo:
                    r, c = all16[idx]; gg[r][c] ^= 1
                st, rid, rot, syn = decode(grid_to_bits16(gg), four_corner)
                if st == 'Ok' and rid != target:
                    if best is None or k < best:
                        best = k; sample = (pr, combo, rid, syn); found_k = True
                    break
            if found_k:
                break
    return best, sample


def main():
    print("=== Phase 0: min-flips to a WRONG id (exhaustive, all 16 cells) ===")
    for fc in (False, True):
        gate = "4-corner" if fc else "3-corner"
        mk, s = exhaustive_min_flips(16, four_corner=fc, maxk=5)
        if mk is None:
            print(f"  {gate}: NO wrong-id read within 5 flips (immune)")
        else:
            pr, combo, rid, syn = s
            print(f"  {gate}: min {mk} flip(s) -> wrong id {rid} (syn={syn}) "
                  f"[phys_rot={pr*90}, cells={combo}]")

    print("\n=== Phase 1: garbage characterization for tag-16 ===")
    for fc in (False, True):
        clean, corr, total = characterize(16, four_corner=fc)
        gate = "4-corner (+!g33)" if fc else "3-corner (current)"
        print(f"\n--- gate: {gate} ---  ({total} corrupted variants)")
        wrong_clean = {i:n for i,n in clean.items() if i != 16}
        wrong_corr  = {i:n for i,n in corr.items()  if i != 16}
        print(f"  correct id16: clean={clean.get(16,0)} corrected={corr.get(16,0)}")
        print(f"  WRONG ids that appeared CLEAN (syn0): {dict(sorted(wrong_clean.items())) or 'NONE'}")
        print(f"  distinct wrong ids total: {len(set(wrong_clean)|set(wrong_corr))}")
    phase2()


if __name__ == "__main__":
    main()
