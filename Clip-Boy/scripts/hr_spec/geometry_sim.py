#!/usr/bin/env py -3
"""Phase B/C GEOMETRY simulator: does 4x4 -> 5x5 raise first-scan-success?

PRELIMINARY / DIRECTIONAL. No 5x5 hardware exists. This compares two TAG
GEOMETRIES in aggregate under the SAME shipped lock policy (vote_lock_model),
using the SAME PSF render physics as reliability_sim.py to ground the per-cell
error rate, then an analytic Monte-Carlo over the error-correcting code + the
vote lock. The one thing that is MODELED (not measured on hardware) is how the
per-cell bit-flip rate scales from 4x4 (2.0 sensor-zones/cell) to 5x5 (1.6
zones/cell). That scaling is made explicit, grounded in the render, and swept in
a sensitivity band. Read docs/hr-phaseBC-geometry-sim.md.

THE PHYSICS (front and center)
------------------------------
When a tag fills the 8x8 FoV, zones-per-cell = 8 / grid_width:
    4x4 -> 2.00 z/cell   5x5 -> 1.60 z/cell   3x3 -> 2.67 z/cell
So 5x5 has MORE data cells (21 vs 12 -> room for a stronger code) but WORSE
per-cell spatial resolution (fewer zones average each cell -> a higher raw
per-cell flip rate per frame). The whole question is whether 5x5's error-
correction headroom beats its higher flip rate. 3x3 is very robust per cell but
has only 5 data cells (9 - 3 anchors - 1 guard) -- far too few for 95 ids + ECC
-- so it is noted and excluded.

CODES
-----
  4x4: 16 - 3 anchors - 1 rotation-guard = 12 data cells.
       SECDED(12,7), distance 4  (imported secded.py; corrects 1, detects 2).
       A(12,5)=32 < 95 so distance-4 is the ceiling for 95 ids at 4x4.
  5x5: 25 - 3 anchors - 1 guard = 21 data cells.
       A computer-SEARCHED, Griesmer-OPTIMAL [21,7,8] binary linear code
       (min distance 8 -> corrects t=3, detects 4). Generator frozen below;
       distance re-verified by exhaustive enumeration at import (self_test).

MODELING DECISIONS (explicit; see doc)
--------------------------------------
1. Anchors (solid pads) + the guard are structural and modeled as NOT flipping
   (empirically the robust part). Anchor/guard/pose failures are folded into a
   single per-frame pose-fail probability applied EQUALLY to both geometries, so
   it never biases the comparison.
2. Per-frame, each DATA cell flips independently at p_cell(W, noise). p_cell is
   MEASURED from the PSF render (measure_pcell) -- the render's finer 5x5 cells
   naturally wash out more, which is what grounds the 4x4->5x5 flip-rate ratio
   in physics rather than a bare guess.
3. Independent per-cell flips understate SPATIALLY-CORRELATED washout (isolated
   cells), which is the main WRONG-lock driver on hardware. Phase A's lock
   policy already mitigates wrong-locks and both geometries share the same
   model, so the LOCK/TIMEOUT comparison stays directional; the wrong-rate is a
   lower bound. The correlated term is added as an optional pose-consistent
   washout (bad_pose) applied equally.

Public: run `py -3 geometry_sim.py` for the full report.
"""
import os
import sys
import math
import random

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from secded import encode as secded_encode, decode as secded_decode
from vote_lock_model import VoteLock

# ===========================================================================
# 5x5 error-correcting code: computer-searched Griesmer-optimal [21,7,8].
# G = [I_7 | P]; low 7 bits = info (systematic), high 14 bits = parity.
# Distance re-verified by exhaustive enumeration in self_test().
# ===========================================================================
_G5 = [
    0b010101101000110000001,
    0b111111111101110000010,
    0b101011101100000000100,
    0b011110101010000001000,
    0b110110100001100010000,
    0b001111010111010100000,
    0b101100111000101000000,
]
_N5 = 21
_K5 = 7

def _bits(x, n):
    return [(x >> i) & 1 for i in range(n)]

# Parity-check H = [P^T | I_(n-k)] as (n-k) row masks over n bit positions.
def _build_H():
    # P: rows i have parity bits in positions 7..20 -> column j (7..20) of P^T
    nk = _N5 - _K5
    H = []
    for j in range(nk):                 # parity bit index j -> code position 7+j
        row = 0
        for i in range(_K5):            # P^T[j][i] = P[i][j] = bit (7+j) of G row i
            if (_G5[i] >> (_K5 + j)) & 1:
                row |= (1 << i)         # couples info position i
        row |= (1 << (_K5 + j))         # identity on parity position
        H.append(row)
    return H

_H5 = _build_H()

def _syndrome5(cw):
    """cw: 21-bit int -> syndrome int (14 bits)."""
    s = 0
    for j, hrow in enumerate(_H5):
        if bin(hrow & cw).count("1") & 1:
            s |= (1 << j)
    return s

def encode5(id7):
    """id7 (0..127) -> 21-bit codeword int."""
    cw = 0
    for i in range(_K5):
        if (id7 >> i) & 1:
            cw ^= _G5[i]
    return cw

# Bounded-distance syndrome table: coset leaders of weight <= T5 (=3).
T5 = 3
def _build_syndrome_table():
    tbl = {0: 0}
    import itertools
    for w in range(1, T5 + 1):
        for combo in itertools.combinations(range(_N5), w):
            e = 0
            for b in combo:
                e |= (1 << b)
            s = _syndrome5(e)
            if s not in tbl:            # first (lowest weight) leader wins
                tbl[s] = e
    return tbl

_SYN_TABLE5 = _build_syndrome_table()

def decode5(cw):
    """21-bit received word -> (id|None, status) where status in
    {'ok','corrected','double'}. 'ok' = syndrome 0 (clean). 'corrected' = a
    weight<=3 coset leader applied. 'double' = syndrome not in the t=3 table
    (>=4 flips detected) -> rejected. Mis-decode to a WRONG id emerges when the
    true error weight >3 but the syndrome collides with a <=3 leader."""
    s = _syndrome5(cw)
    if s == 0:
        idv = cw & ((1 << _K5) - 1)
        return idv, 'ok'
    e = _SYN_TABLE5.get(s)
    if e is None:
        return None, 'double'
    corr = cw ^ e
    idv = corr & ((1 << _K5) - 1)
    return idv, 'corrected'


def self_test():
    """Verify the 5x5 code round-trips with 0,1,2,3 injected flips and report
    min distance + double-detect integrity. Called at import."""
    import itertools
    # min distance by exhaustive enumeration of all 128 codewords
    dmin = 99
    for m in range(1, 1 << _K5):
        cw = 0
        for i in range(_K5):
            if (m >> i) & 1:
                cw ^= _G5[i]
        w = bin(cw).count("1")
        if w < dmin:
            dmin = w
    assert dmin == 8, "5x5 code distance regressed: %d" % dmin
    # 0..3 flips must recover the id
    for idv in range(128):
        base = encode5(idv)
        for k in range(0, T5 + 1):
            for combo in itertools.combinations(range(_N5), k):
                cw = base
                for b in combo:
                    cw ^= (1 << b)
                did, st = decode5(cw)
                assert did == idv, ("5x5 miscorrect id=%d k=%d combo=%s got=%s"
                                    % (idv, k, combo, did))
    return dmin

_D5 = self_test()

# also confirm the 4x4 reference is sane
assert secded_decode(secded_encode(37)) == (37, 'ok')


# ===========================================================================
# GEOMETRY DESCRIPTORS
# ===========================================================================
class Geom:
    def __init__(self, W, ndata, code_name, dmin, t_corr):
        self.W = W
        self.z_per_cell = 8.0 / W
        self.ndata = ndata
        self.code_name = code_name
        self.dmin = dmin
        self.t_corr = t_corr
        # data-cell list (row-major, excluding 3 anchors + BR guard)
        anchors = {(0, 0), (0, W - 1), (W - 1, 0)}
        guard = (W - 1, W - 1)
        self.data_cells = [(r, c) for r in range(W) for c in range(W)
                           if (r, c) not in anchors and (r, c) != guard]
        assert len(self.data_cells) == ndata
        self.anchors = anchors
        self.guard = guard

G4 = Geom(4, 12, "SECDED(12,7)", 4, 1)
G5 = Geom(5, 21, "[21,7,8] searched", _D5, T5)

def geom_encode(g, idv):
    """id -> list of ndata bits for geometry g."""
    if g.W == 4:
        return secded_encode(idv)          # 12 bits
    cw = encode5(idv)
    return _bits(cw, _N5)

def geom_decode(g, databits):
    """list of ndata bits -> (id|None, status in {'ok','corrected','double'})."""
    if g.W == 4:
        return secded_decode(databits)
    cw = 0
    for i, b in enumerate(databits):
        if b:
            cw |= (1 << i)
    return decode5(cw)


# ===========================================================================
# PSF RENDER (W-parameterized) -- reuses reliability_sim's physics to MEASURE
# the per-cell flip rate for each geometry at a given sensor-noise sigma.
# ===========================================================================
class RP:
    near_mm = 80.0
    step_mm = 24.0
    psf_sigma = 0.60        # zones; the LiDAR lateral resolution / crosstalk
    psf_taps = 7
    bump_fill = 0.82        # fraction of a cell pitch a bump pad occupies
    fill_lo = 6.6
    fill_hi = 7.4
    off_jit = 0.45
    tilt_max = 0.16
    pose_jit = 0.13         # residual pose translation the calib can't remove (zones)
    pose_scale_jit = 0.04

_TAP_OFF = None
_TAP_W = None
def _tap_cache():
    global _TAP_OFF, _TAP_W
    if _TAP_OFF is None:
        span = 2.2 * RP.psf_sigma
        offs, ws = [], []
        for t in range(RP.psf_taps):
            o = -span + (2.0 * span) * (t + 0.5) / RP.psf_taps
            offs.append(o)
            ws.append(math.exp(-0.5 * (o / RP.psf_sigma) ** 2))
        _TAP_OFF = np.array(offs)
        _TAP_W = np.array(ws)
    return _TAP_OFF, _TAP_W

def _coverage_grid(U, V, grid, W, anchors, guard):
    """Vectorized bump-coverage at unit-tag points U,V (numpy arrays). grid is
    a W x W int array (1=bump). Returns coverage in [0,1]. Same asymmetric model
    as reliability_sim: recessed FAR reads far robustly; a raised bump reads near
    only where PSF-integrated coverage is high, with a moat margin around each
    data pad (anchors are solid full-cell pads)."""
    cov = np.zeros_like(U)
    inside = (U >= 0.0) & (U < 1.0) & (V >= 0.0) & (V < 1.0)
    c = np.clip((U * W).astype(int), 0, W - 1)
    r = np.clip((V * W).astype(int), 0, W - 1)
    val = np.zeros_like(U, dtype=int)
    val[inside] = grid[r[inside], c[inside]]
    isbump = inside & (val == 1)
    # anchor cells: solid pad -> full coverage
    anchor_mask = np.zeros_like(U, dtype=bool)
    for (ar, ac) in anchors:
        anchor_mask |= isbump & (r == ar) & (c == ac)
    cov[anchor_mask] = 1.0
    # data bump pads: moat margin
    databump = isbump & (~anchor_mask)
    m = (1.0 - RP.bump_fill) * 0.5
    fu = U * W - c
    fv = V * W - r
    in_pad = (fu >= m) & (fu <= 1.0 - m) & (fv >= m) & (fv <= 1.0 - m)
    cov[databump & in_pad] = 1.0
    return cov

def render_depth(grid, W, ox, oy, fill, tu, tv, noise, anchors, guard, rng):
    """Return 8x8 depth (numpy). PSF-weighted coverage integral per zone."""
    offs, ws = _tap_cache()
    far = RP.near_mm + RP.step_mm
    # zone centers
    zr = np.arange(8) + 0.5
    zc = np.arange(8) + 0.5
    ZR, ZC = np.meshgrid(zr, zc, indexing='ij')   # 8x8
    cov = np.zeros((8, 8))
    tilt = np.zeros((8, 8))
    wsum = 0.0
    for ti in range(len(offs)):
        wr = ws[ti]
        rr = ZR + offs[ti]
        for tj in range(len(offs)):
            w = wr * ws[tj]
            cc = ZC + offs[tj]
            U = (cc - ox) / fill
            V = (rr - oy) / fill
            cov += w * _coverage_grid(U, V, grid, W, anchors, guard)
            tilt += w * (tu * (U - 0.5) + tv * (V - 0.5))
            wsum += w
    cov /= wsum
    tilt /= wsum
    depth = far - RP.step_mm * cov + tilt
    depth += rng.normal(0.0, noise, size=(8, 8))
    return depth

def _kmeans_thr(depths):
    lo, hi = float(np.min(depths)), float(np.max(depths))
    c1, c2 = lo, hi
    for _ in range(16):
        d1 = np.abs(depths - c1)
        d2 = np.abs(depths - c2)
        m1 = d1 <= d2
        if m1.all() or (~m1).all():
            break
        n1 = depths[m1].mean()
        n2 = depths[~m1].mean()
        if abs(n1 - c1) < 0.5 and abs(n2 - c2) < 0.5:
            c1, c2 = n1, n2
            break
        c1, c2 = n1, n2
    if c1 > c2:
        c1, c2 = c2, c1
    return c1, c2

def _grid_for(g, idv):
    """W x W truth grid: anchors=1 at 3 corners, guard=0 BR, data from code."""
    W = g.W
    grid = np.zeros((W, W), dtype=int)
    for (ar, ac) in g.anchors:
        grid[ar, ac] = 1
    grid[g.guard] = 0
    bits = geom_encode(g, idv)
    for b, (r, c) in zip(bits, g.data_cells):
        grid[r, c] = b
    return grid

def measure_pcell(g, noise, n=400, seed=7, ids=None):
    """MEASURE the mean per-data-cell flip probability for geometry g at sensor
    noise sigma (mm), by rendering random poses of random codewords and reading
    each data cell back via KNOWN-pose per-cell majority (the calibrated affine
    recovers pose on real hardware; a small residual jitter is added). Returns
    (p_cell, frac_pose_fail)."""
    rng = np.random.default_rng(seed * 131 + int(noise * 100) + g.W)
    pyrng = random.Random(seed * 977 + g.W)
    W = g.W
    if ids is None:
        ids = [pyrng.randint(1, 100) for _ in range(n)]
    else:
        ids = [ids[i % len(ids)] for i in range(n)]
    flips = 0
    total = 0
    posefail = 0
    for t in range(n):
        idv = ids[t]
        grid = _grid_for(g, idv)
        fill = rng.uniform(RP.fill_lo, RP.fill_hi)
        base = (8.0 - fill) / 2.0
        ox = base + rng.uniform(-RP.off_jit, RP.off_jit)
        oy = base + rng.uniform(-RP.off_jit, RP.off_jit)
        tmag = rng.uniform(0.0, RP.tilt_max)
        ang = rng.uniform(0, 2 * math.pi)
        tu, tv = tmag * math.cos(ang), tmag * math.sin(ang)
        depth = render_depth(grid, W, ox, oy, fill, tu, tv, noise,
                             g.anchors, g.guard, rng)
        d = depth.flatten()
        med = np.median(d)
        inl = d[np.abs(d - med) <= 25.0]
        if inl.size < 32:
            posefail += 1
            continue
        c1, c2 = _kmeans_thr(inl)
        sep = c2 - c1
        if sep < 8.0 or sep > 30.0:
            posefail += 1
            continue
        thr = 0.5 * (c1 + c2)
        # known pose + small residual jitter
        jr = rng.normal(0.0, RP.pose_jit)
        jc = rng.normal(0.0, RP.pose_jit)
        js = 1.0 + rng.normal(0.0, RP.pose_scale_jit)
        cx = ox + 0.5 * fill
        cy = oy + 0.5 * fill
        inv = 1.0 / (fill * js)
        near = depth <= thr
        cellNear = np.zeros(W * W)
        cellTot = np.zeros(W * W)
        for r in range(8):
            for c in range(8):
                u = ((c + 0.5) - jc - cx) * inv + 0.5
                v = ((r + 0.5) - jr - cy) * inv + 0.5
                if u < 0.0 or u >= 1.0 or v < 0.0 or v >= 1.0:
                    continue
                if abs(depth[r, c] - thr) < 2.0:
                    continue                       # deadband
                ci = int(v * W) * W + int(u * W)
                cellTot[ci] += 1
                if near[r, c]:
                    cellNear[ci] += 1
        # compare each DATA cell's recovered bit to truth
        for (r, c) in g.data_cells:
            ci = r * W + c
            if cellTot[ci] == 0:
                flips += 1                          # unread data cell = effective error
                total += 1
                continue
            bit = 1 if (cellNear[ci] * 2 > cellTot[ci]) else 0
            if bit != grid[r, c]:
                flips += 1
            total += 1
    p = flips / total if total else 0.0
    return p, posefail / n


# ===========================================================================
# ANALYTIC MONTE-CARLO: per-frame iid data-cell flips -> code decode -> VoteLock.
# ===========================================================================
CATALOG = [i for i in range(1, 101) if i not in {11, 59, 82, 91, 94}]  # 95 ids

def run_scan(g, p_cell, p_posefail, max_frames, rng,
             lock_win=24, lock_floor=12, lock_margin=20,
             bad_pose_frac=0.0, bad_pose_len=(6, 16), idv=None):
    """One scan of a tag. Returns ('LOCK'|'WRONG'|'TIMEOUT', frames_used)."""
    if idv is None:
        idv = rng.choice(CATALOG)
    truth = geom_encode(g, idv)
    vl = VoteLock(window=lock_win, floor=lock_floor, margin_x10=lock_margin)
    # optional correlated "bad pose": a dwell during which a consistent cluster
    # of cells is corrupted (models isolated-cell washout / affine snap). Same
    # generative model for both geometries.
    bad_frames_left = 0
    bad_mask = None
    for f in range(max_frames):
        if rng.random() < p_posefail:
            vl.feed(-1, False, pose_ok=False)      # dropped frame, no vote
            continue
        if bad_frames_left == 0 and rng.random() < bad_pose_frac:
            bad_frames_left = rng.randint(*bad_pose_len)
            # corrupt a contiguous-ish random subset (~30-45% of data cells)
            k = max(2, int(round(g.ndata * rng.uniform(0.30, 0.45))))
            bad_mask = set(rng.sample(range(g.ndata), k))
        databits = list(truth)
        for i in range(g.ndata):
            flip = rng.random() < p_cell
            if bad_frames_left > 0 and i in bad_mask:
                flip = True                        # systematic during the bad dwell
            if flip:
                databits[i] ^= 1
        if bad_frames_left > 0:
            bad_frames_left -= 1
        did, st = geom_decode(g, databits)
        if st == 'double' or did is None:
            # Detected/rejected frame: contributes NO vote (VoteLock only tallies
            # decoded candidates). It does not age the ring either -- a rejected
            # frame is simply a lost frame, matching the firmware.
            continue
        clean = (st == 'ok')
        locked = vl.feed(did, clean, pose_ok=True)
        if locked is not None:
            return ('LOCK' if locked == idv else 'WRONG'), f + 1
    return 'TIMEOUT', max_frames

def montecarlo(g, p_cell, p_posefail, trials=4000, max_frames=40, seed=1,
               bad_pose_frac=0.0):
    rng = random.Random(seed * 31 + g.W)
    lock = wrong = to = 0
    ttls = []
    for _ in range(trials):
        out, fr = run_scan(g, p_cell, p_posefail, max_frames, rng,
                           bad_pose_frac=bad_pose_frac)
        if out == 'LOCK':
            lock += 1
            ttls.append(fr)
        elif out == 'WRONG':
            wrong += 1
        else:
            to += 1
    return {
        'lock_rate': lock / trials,
        'wrong_rate': wrong / trials,
        'timeout_rate': to / trials,
        'mean_frames': (sum(ttls) / len(ttls)) if ttls else float('nan'),
        'n': trials,
    }


# ===========================================================================
# REPORT
# ===========================================================================
# Frame->seconds: hardware mean_ttl 5.8s at typical ~13-17 processed frames to
# lock -> ~0.35 s/frame. Used only to annotate frames-to-lock.
SEC_PER_FRAME = 0.35
HW_ANCHOR_LOCK = 0.947          # observed on-catalog lock rate (sweep_summary)

def calibrate_noise(target=HW_ANCHOR_LOCK, trials=3000):
    """Find the render noise sigma at which the 4x4 pipeline's simulated lock
    rate matches the hardware anchor. Returns (noise, p_cell4, res4)."""
    best = None
    for noise in [round(x, 2) for x in np.arange(0.6, 4.01, 0.2)]:
        p4, pf4 = measure_pcell(G4, noise, n=400)
        res = montecarlo(G4, p4, pf4, trials=trials, bad_pose_frac=0.04)
        err = abs(res['lock_rate'] - target)
        if best is None or err < best[0]:
            best = (err, noise, p4, pf4, res)
    return best

def main():
    print("=" * 74)
    print("HR TAG GEOMETRY SIM (Phase B/C)  --  PRELIMINARY / DIRECTIONAL")
    print("=" * 74)
    print("5x5 code: [21,7,%d]  corrects t=%d, detects %d  (Griesmer-optimal)"
          % (_D5, T5, _D5 - 1 - T5))    # simultaneous correct-t / detect-s: t+s<d
    print("4x4 code: SECDED(12,7) d=4  corrects 1, detects 2")
    print("zones/cell: 4x4=%.2f  5x5=%.2f  3x3=%.2f (3x3 excluded: only 5 data cells)"
          % (G4.z_per_cell, G5.z_per_cell, 8.0 / 3))
    print()

    # --- per-cell flip rate vs noise, both geometries (render-measured) ---
    print("RENDER-MEASURED per-cell flip prob  p_cell(W, noise)  [physics grounding]")
    print("  %-8s %-14s %-14s %-8s" % ("noise", "4x4 p_cell", "5x5 p_cell", "ratio"))
    noises = [0.6, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0]
    pcell = {}
    for nz in noises:
        p4, pf4 = measure_pcell(G4, nz, n=500)
        p5, pf5 = measure_pcell(G5, nz, n=500)
        pcell[nz] = (p4, pf4, p5, pf5)
        ratio = (p5 / p4) if p4 > 0 else float('nan')
        print("  %-8.2f %-14.4f %-14.4f %-8.2f" % (nz, p4, p5, ratio))
    ratios = [pcell[nz][2] / pcell[nz][0] for nz in noises if pcell[nz][0] > 0]
    base_ratio = sum(ratios) / len(ratios)
    print("  mean render 5x5/4x4 flip-rate ratio = %.2f  (BASELINE scaling assumption)"
          % base_ratio)
    print()

    # --- calibration ---
    print("CALIBRATION to hardware anchor (on-catalog lock = %.1f%%):" % (HW_ANCHOR_LOCK * 100))
    err, cal_noise, cal_p4, cal_pf4, cal_res = calibrate_noise()
    print("  best-fit render noise sigma = %.2f mm" % cal_noise)
    print("  -> 4x4 p_cell = %.4f, pose_fail = %.3f" % (cal_p4, cal_pf4))
    print("  -> 4x4 sim lock = %.1f%% (target %.1f%%, |err|=%.3f), wrong = %.1f%%, "
          "mean frames = %.1f (~%.1fs)"
          % (cal_res['lock_rate'] * 100, HW_ANCHOR_LOCK * 100, err,
             cal_res['wrong_rate'] * 100, cal_res['mean_frames'],
             cal_res['mean_frames'] * SEC_PER_FRAME))
    print()

    # --- crossover table over noise (both geometries) ---
    print("CROSSOVER TABLE: first-scan-success + wrong-rate vs render noise")
    print("  %-7s | %-26s | %-26s" % ("noise", "4x4 SECDED(12,7)", "5x5 [21,7,8]"))
    print("  %-7s | %-8s %-8s %-8s | %-8s %-8s %-8s"
          % ("sigma", "lock%", "wrong%", "frames", "lock%", "wrong%", "frames"))
    print("  " + "-" * 70)
    cross = []
    for nz in noises:
        p4, pf4, p5, pf5 = pcell[nz]
        r4 = montecarlo(G4, p4, pf4, trials=6000, bad_pose_frac=0.04)
        r5 = montecarlo(G5, p5, pf5, trials=6000, bad_pose_frac=0.04)
        cross.append((nz, r4, r5))
        star = "  <= CAL" if abs(nz - cal_noise) < 0.11 else ""
        print("  %-7.2f | %-8.1f %-8.1f %-8.1f | %-8.1f %-8.1f %-8.1f%s"
              % (nz, r4['lock_rate'] * 100, r4['wrong_rate'] * 100, r4['mean_frames'],
                 r5['lock_rate'] * 100, r5['wrong_rate'] * 100, r5['mean_frames'], star))
    print()

    # --- sensitivity band: scale 5x5 p_cell by an uncertainty factor ---
    print("SENSITIVITY BAND (5x5 flip rate is MODELED, not hw-verified):")
    print("  at calibration noise=%.2f, 4x4 lock=%.1f%%; sweep a multiplier on the"
          % (cal_noise, cal_res['lock_rate'] * 100))
    print("  render 5x5 p_cell to bracket the extrapolation uncertainty:")
    # p5 at cal noise (interp/measure)
    p5c, pf5c = measure_pcell(G5, cal_noise, n=600)
    print("  %-10s %-10s %-10s %-10s %-10s" % ("mult", "5x5 p_cell", "lock%", "wrong%", "frames"))
    for mult in [0.7, 0.85, 1.0, 1.15, 1.3, 1.5, 1.75, 2.0]:
        r5 = montecarlo(G5, min(p5c * mult, 0.49), pf5c, trials=8000, bad_pose_frac=0.04)
        print("  %-10.2f %-10.4f %-10.1f %-10.1f %-10.1f"
              % (mult, p5c * mult, r5['lock_rate'] * 100, r5['wrong_rate'] * 100,
                 r5['mean_frames']))
    print("  (4x4 baseline at cal = %.1f%% lock)" % (cal_res['lock_rate'] * 100))
    print()
    print("Full analysis + recommendation: docs/hr-phaseBC-geometry-sim.md")


if __name__ == "__main__":
    main()
