#!/usr/bin/env python3
"""DC34-155 F1 fix validation: pose-based rotation decode under guard-bleed.

The review found the old guard-far/lowest-index rotation tiebreak was UNSOUND:
when the true rotation's guard cell (3,3) bleeds NEAR (adjacent data bumps), a
WRONG rotation can decode a CLEAN codeword and win (id-16 -> a clean id-2 at
90/180 -> wrong lock). The prior sim only forced guard-near at the canonical
pose (pr==0), so it never exercised rotated poses.

Fix: the localization's WEAKEST bbox corner IS the tag's guard, which fixes the
true de-rotation (`expectedRot`) from the POSE, independent of the guard cell
value. decodeAnchorBits then decodes ONLY that rotation.

This models the full path across ALL rotated poses x {guard bled near, guard
far} and confirms 0 wrong -- the case the old sim missed.
"""
import sys
sys.path.insert(0, r'C:\Users\data\OneDrive\esp\libraries\HRCode4x4\examples')
import hm_codegen as H

DR = [0,0,1,1,1,1,2,2,2,2,3,3]
DC = [1,2,0,1,2,3,0,1,2,3,1,2]
PPOS = [1,2,4,8]
DPOS = [3,5,6,7,9,10,11]
K_ROT_FOR_MISS = [2, 1, 3, 0]   # miss bbox corner TL,TR,BL,BR -> CW de-rotation (matches HRScanEngine.cpp)


def rot(g):
    return [[g[3-c][r] for c in range(4)] for r in range(4)]


def rotpos(r, c):        # where cell (r,c) lands after one 90CW grid rotation
    return (c, 3 - r)


def corner_idx(r, c):
    return {(0, 0): 0, (0, 3): 1, (3, 0): 2, (3, 3): 3}[(r, c)]


def decode_at(base, t):
    """SECDED-decode `base` rotated by t (the shipped single-rotation path)."""
    g = [row[:] for row in base]
    for _ in range(t):
        g = rot(g)
    if not (g[0][0] and g[0][3] and g[3][0]):
        return ('gate', 0)
    h = [0] * 12
    for i in range(11):
        h[i + 1] = g[DR[i]][DC[i]]
    ov = g[DR[11]][DC[11]]
    syn = 0
    for p in PPOS:
        s = 0
        for j in range(1, 12):
            if j & p:
                s ^= h[j]
        if s:
            syn |= p
    c = ov
    for j in range(1, 12):
        c ^= h[j]
    if syn == 0 and c == 0:
        pass
    elif c == 1:
        if 1 <= syn <= 11:
            h[syn] ^= 1
    else:
        return ('double', 0)
    rid = 0
    for i in range(7):
        rid |= h[DPOS[i]] << i
    return ('ok', rid)


def main():
    bad = 0
    examples = []
    for i in range(128):
        can = H.build_anchor_grid(i)
        for bleed in (0, 1):                       # guard reads far (0) or bleeds near (1)
            canb = [row[:] for row in can]
            canb[3][3] = bleed
            for pr in range(4):                    # physical tag rotation
                bits = [row[:] for row in canb]
                for _ in range(pr):
                    bits = rot(bits)
                # the localization's weakest corner = the guard's position in the bbox frame
                gr, gc = 3, 3
                for _ in range(pr):
                    gr, gc = rotpos(gr, gc)
                miss = corner_idx(gr, gc)
                expected_rot = K_ROT_FOR_MISS[miss]
                st, rid = decode_at(bits, expected_rot)
                if not (st == 'ok' and rid == i):
                    bad += 1
                    if len(examples) < 6:
                        examples.append((i, bleed, pr, st, rid))
    total = 128 * 2 * 4
    print(f"pose-based decode over {total} cases (128 ids x {{far,bled guard}} x 4 rotations):")
    print(f"  wrong/failed: {bad}  {examples if examples else '(all correct)'}")
    assert bad == 0
    print("PASS -- rotation disambiguation sound under guard-bleed at every pose")


if __name__ == "__main__":
    main()
