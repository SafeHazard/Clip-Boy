#!/usr/bin/env python3
"""Bit-exact model of the C decodeAnchorBits() (SECDED(12,7) + rotation gate).

Mirrors HRScanEngine.cpp: same DR/DC/PPOS/DPOS, same 4-rotation CW loop, same
anchor gate, same SECDED decode. Cross-checked against hm_codegen.build_anchor_grid
so a port bug shows up here, not on hardware.
"""
import sys
sys.path.insert(0, r'C:\Users\data\OneDrive\esp\libraries\HRCode4x4\examples')
import hm_codegen as H

DR = [0,0,1,1,1,1,2,2,2,2,3,3]
DC = [1,2,0,1,2,3,0,1,2,3,1,2]
PPOS = [1,2,4,8]
DPOS = [3,5,6,7,9,10,11]


def rot90cw(g):
    # out[r][c] = in[3-c][r]
    return [[g[3-c][r] for c in range(4)] for r in range(4)]


def decode_anchor(bits16):
    """bits16: 16-list (row-major). Returns (status, id, rotation)."""
    base = [[bits16[r*4+c] & 1 for c in range(4)] for r in range(4)]
    status = 'NoOrient'; rid = 0; rot = 0
    for t in range(4):
        g = [row[:] for row in base]
        for _ in range(t):
            g = rot90cw(g)
        if not (g[0][0] and g[0][3] and g[3][0]):
            continue
        if status == 'NoOrient':
            status = 'BadCRC'
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
        rot = t
        if good:
            return ('Ok', rid, rot)
    return (status, rid, rot)


def grid_to_bits16(g):
    return [g[r][c] for r in range(4) for c in range(4)]


def main():
    # 1. clean decode, all IDs, all 4 physical rotations
    bad = 0; wrongrot = 0
    for i in range(128):
        g = H.build_anchor_grid(i)
        for pr in range(4):            # physical rotation of the tag
            pg = g
            for _ in range(pr): pg = rot90cw(pg)
            st, rid, rot = decode_anchor(grid_to_bits16(pg))
            if st != 'Ok' or rid != i:
                bad += 1
    print("clean decode (128 ids x 4 rotations) failures:", bad)

    # 2. single-cell flip on the 12 data cells (canonical orientation) -> corrected
    cells12 = [(DR[k], DC[k]) for k in range(12)]
    mis_s = 0
    for i in range(128):
        g = H.build_anchor_grid(i)
        for (r,c) in cells12:
            gg = [row[:] for row in g]; gg[r][c] ^= 1
            st, rid, rot = decode_anchor(grid_to_bits16(gg))
            if st != 'Ok' or rid != i:
                mis_s += 1
    print("single-cell-flip miscorrects:", mis_s)

    # 3. double-cell flip on the 12 data cells -> must NOT decode to a wrong id
    mis_d = 0
    for i in range(128):
        g = H.build_anchor_grid(i)
        for a in range(12):
            for b in range(a+1,12):
                gg = [row[:] for row in g]
                gg[cells12[a][0]][cells12[a][1]] ^= 1
                gg[cells12[b][0]][cells12[b][1]] ^= 1
                st, rid, rot = decode_anchor(grid_to_bits16(gg))
                if st == 'Ok' and rid != i:
                    mis_d += 1
    print("double-cell-flip mis-decodes (Ok + wrong id):", mis_d)

    assert bad == 0 and mis_s == 0 and mis_d == 0
    print("FIRMWARE MODEL PASS")


if __name__ == "__main__":
    main()
