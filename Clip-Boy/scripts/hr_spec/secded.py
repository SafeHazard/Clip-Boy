#!/usr/bin/env python3
"""SECDED(12,7) reference for the HR anchor spec.

The anchor layout has 13 data cells, but cell (3,3) must stay 0 as a ROTATION
guard (a 0 in the BR corner is what makes the 3 wrong rotations fail the
3-anchor gate in decodeAnchorBits — otherwise SECDED's large accept region
would false-accept a wrong-rotation read and unlock the wrong collectible).

So the code lives in the OTHER 12 cells: extended Hamming(11,7) + 1 overall
parity = SECDED(12,7), distance 4. 7-bit id covers 0..127 (collectibles are
1..100). Corrects any single-cell flip (the ~1-cell/frame VL53L5CX flicker) so
the read locks; detects (rejects) any double-cell flip so a corrupted read
never unlocks the wrong id.

Codeword bit order into the 12 row-major data cells (all data cells EXCEPT the
(3,3) guard): cw[0..10] = Hamming positions 1..11, cw[11] = overall parity.
"""

PPOS = [1, 2, 4, 8]                    # parity positions (powers of two)
DPOS = [3, 5, 6, 7, 9, 10, 11]        # the 7 data positions in 1..11


def encode(id7):
    """id7 (0..127) -> list of 12 codeword bits."""
    h = [0] * 12                       # h[1..11] used, h[0] ignored
    for i in range(7):
        h[DPOS[i]] = (id7 >> i) & 1
    for p in PPOS:                     # even parity over covered positions
        s = 0
        for j in range(1, 12):
            if j != p and (j & p):
                s ^= h[j]
        h[p] = s
    overall = 0
    for j in range(1, 12):
        overall ^= h[j]
    return [h[j] for j in range(1, 12)] + [overall]


def decode(cw):
    """12 bits -> (id, status) where status in {'ok','corrected','double'}."""
    h = [0] + [cw[i] for i in range(11)]   # h[1..11]
    stored_overall = cw[11]
    syn = 0
    for p in PPOS:
        s = 0
        for j in range(1, 12):
            if j & p:
                s ^= h[j]
        if s:
            syn |= p
    c = stored_overall
    for j in range(1, 12):
        c ^= h[j]
    if syn == 0 and c == 0:
        status = 'ok'
    elif c == 1:
        if 1 <= syn <= 11:
            h[syn] ^= 1
        status = 'corrected'           # (syn==0 -> the overall bit flipped; data ok)
    else:
        return (None, 'double')
    id7 = 0
    for i in range(7):
        id7 |= h[DPOS[i]] << i
    return (id7, status)


def _self_test():
    N = 128
    for i in range(N):
        assert decode(encode(i)) == (i, 'ok'), (i, decode(encode(i)))
    mis_s = 0
    for i in range(N):
        base = encode(i)
        for b in range(12):
            cw = base[:]; cw[b] ^= 1
            if decode(cw)[0] != i:
                mis_s += 1
    mis_d = 0
    for i in range(N):
        base = encode(i)
        for a in range(12):
            for b in range(a + 1, 12):
                cw = base[:]; cw[a] ^= 1; cw[b] ^= 1
                idr, st = decode(cw)
                if st != 'double' and idr != i:
                    mis_d += 1
    print("round-trip: OK (%d/%d)" % (N, N))
    print("single-cell miscorrects:", mis_s)
    print("double-cell mis-decodes (accepted+wrong id):", mis_d)
    assert mis_s == 0 and mis_d == 0
    print("PASS")


if __name__ == "__main__":
    _self_test()
