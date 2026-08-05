#!/usr/bin/env py -3
"""Monte-Carlo reliability simulator for the LiDAR "HR code" scanner.

⚠⚠ DO NOT TRUST THIS TO PICK RELIABLE IDs. ⚠⚠  (Jul 2026, learned the hard way.)
Physical reliability is dominated by print-registration + per-zone sensor response,
which this model does NOT capture. Proven wrong repeatedly: ID 91 scored 0.70 and was
physically ROUGH; ID 11 scored 0.84 (highest) and physically ALIASED to 15. The ONLY
trustworthy signal is a tag PHYSICALLY self-decoding -- use `id_sweep.py <id>` (fixed to
poll hr_debug) on the actual printed/OmniTag tag. Prefer all-cells-near / iso=0 patterns,
then VERIFY. This sim is at best a weak negative filter for obvious checkerboards.
See docs/hr-reliability-findings.md.

For every tag ID 1..127 this predicts how RELIABLY the physical tag self-decodes
on real hardware, by rendering many randomized 8x8 LiDAR depth frames (random
alignment / distance / tilt / sensor noise) and running each frame through a
faithful re-implementation of the firmware decode pipeline
(HRScanGuidance/src/HRScanEngine.cpp :: processFrameCV + decodeAnchorBits).

reliability(id) = fraction of trials that produce a CLEAN self-decode:
    decoded.status == Ok  AND  syndrome == 0  AND  decoded.id == id
That is exactly what the firmware needs to LOCK a tag (it requires >= 2 clean
syndrome-0 reads of the same id). A frame that decodes to a WRONG id, or only
ever "corrected" (syndrome != 0), or is rejected by a gate, does NOT count.

The model is calibrated so its ranking matches known hardware results:
    id 101 -> FAILS  (samples 1001/1001/0001/1001, decodes to 37, never clean)
    id 43  -> FAILS  (BadCRC, interior scrambles)
    ids 47,55,88,37,64,73 -> RELIABLE (self-decode clean, lock fast)
    id 16  -> MARGINAL (self-decodes but only occasionally clean)

Pipeline modelled (faithful to the firmware):
  1. Render 8x8 depth: bump cell = NEAR (small mm), flat = FAR (+step mm). A cell
     spans ~1.75 zones; zones straddling a near/far boundary average the two
     (supersampled render -> this straddle-aliasing is what misreads some tags).
  2. Median inlier filter (+/-25mm), k-means(k=2) near/far threshold.
  3. Separation gate, near classify with +/-2mm deadband, bbox of NEAR zones,
     pad small dim to 6.
  4. GRADED WEIGHTED RING anchor centroids (Chebyshev weights 3/2/1) at all 4
     bbox corners. Guard PINNED to BR; the 3 anchors are TL/TR/BL.
  5. Affine (3 anchor centroids -> unit-cell u,v), sample each zone center
     through it, per-cell NEAR majority -> 16 bits.
  6. SECDED(12,7) decode at the pinned rotation -> (id, syndrome).

Public API:
    reliability(id, n=..., seed=...) -> float in [0,1]
    rank_all(n=..., seed=...)        -> list of (id, score) sorted desc
    sample_ideal_bits(id)            -> the 4x4 bit grid a well-aligned frame reads
"""
import os
import sys
import math
import random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from secded import decode as secded_decode          # (cw12) -> (id|None, 'ok'|'corrected'|'double')
from pattern_hardness import grid_for                # grid_for(id) -> 4x4 (anchors+guard+data)

# ---------------------------------------------------------------------------
# Cell order for the 12 data cells (row-major, excluding the (3,3) guard).
# Matches HRScanEngine.cpp DR/DC and secded.py.
# ---------------------------------------------------------------------------
CELL_ORDER = [(0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (1, 3),
              (2, 0), (2, 1), (2, 2), (2, 3), (3, 1), (3, 2)]

_ANCHOR_CELLS = {(0, 0), (0, 3), (3, 0)}       # solid registration pads

# ---------------------------------------------------------------------------
# Firmware gate constants (HRScanEngine.h).
# ---------------------------------------------------------------------------
CV_MIN_VALID_ZONES = 32
CV_SEP_MIN_MM = 8.0
CV_SEP_MAX_MM = 30.0
CV_BBOX_MIN = 3
CV_BBOX_MAX = 8
CV_BBOX_PAD_TO = 6
DEADBAND_MM = 2.0
MIN_NEAR_DENSITY_PCT = 15
MIN_VALID_CELLS = 12

# ---------------------------------------------------------------------------
# Render / sensor model parameters (the calibrated knobs).
#   Distances in mm. NEAR = bump top, FAR = recessed flat (+STEP_MM).
#   FILL   = how many sensor zones the 4-cell tag spans (fills the ~7 of 8 FoV;
#            ~1.75 zones/cell). Smaller FILL -> more aliasing (cells straddle).
#   SS     = supersample grid per zone (models the finite zone footprint that
#            averages a near/far boundary within one zone -> straddle aliasing).
# ---------------------------------------------------------------------------
class Params:
    near_mm = 80.0
    step_mm = 24.0           # far = near + step (physical bump-to-moat depth)
    # PSF: the LiDAR zone integrates over a footprint WIDER than one zone
    # (~1.75-zone lateral resolution + optical crosstalk). Modeled as a Gaussian
    # point-spread with sigma in ZONE units. This is the crux: an ISOLATED near
    # cell (a bump differing from all 4 neighbours, ~1.75 zones wide) gets its
    # peak DILUTED by the surrounding far plane and reads flat -> the exact
    # washout that fails id 101 / 43 on hardware. An extended near region keeps
    # its centre near. Bigger sigma -> more isolated-cell washout.
    psf_sigma = 0.60         # zones
    psf_taps = 7             # samples per axis across the PSF (+/- 2.2 sigma);
    #                          <7 undersamples the coverage integral -> over-wash
    # A bump is a raised PAD that fills only `bump_fill` of the cell pitch,
    # centred, with the recessed moat filling the rest. So an ISOLATED bump is a
    # small near island the PSF easily loses; ADJACENT bumps sit close enough
    # that the PSF bridges their moat gap into one solid near region that
    # survives. Flat cells are full-cell far (the moat). This asymmetry (small
    # near pads vs full far) is what lets clusters read near while lone bumps and
    # tag-perimeter bumps wash to far -- the mechanism that fails id 101/43.
    bump_fill = 0.82         # fraction of the cell pitch a bump pad occupies
    # random alignment jitter ranges (per trial)
    fill_lo = 6.6
    fill_hi = 7.4
    off_jit = 0.45           # +/- zones of translation around centered
    tilt_max = 0.16          # max depth-gradient magnitude, mm per unit-tag
    noise_lo = 0.3           # per-zone gaussian sigma (mm), low
    noise_hi = 0.9           # per-zone gaussian sigma (mm), high
    pose_jit = 0.13          # per-read residual pose translation (zones, 1-sigma)
    pose_scale_jit = 0.04    # residual pose scale error (fraction, 1-sigma)
    pose_flicker = 0.04      # per-frame pose flicker within a read (zones)
    frames = 5               # frames per read (firmware kCvVoteHistory)

P = Params()


# ---------------------------------------------------------------------------
# Height field: unit-tag coords (u=col axis, v=row axis) -> depth (mm).
# grid[r][c]==1 is a bump (NEAR); 0 is flat (FAR). Outside [0,1] -> background
# far plane (the tag sits on / is surrounded by the recessed moat).
# ---------------------------------------------------------------------------
def _coverage(u, v, grid):
    """Bump-coverage indicator at unit-tag point (u,v): 1.0 over a raised bump
    pad, 0.0 over flat/moat/background. The physical asymmetry lives here --
    depth is computed as far - step*PSF(coverage), so a recessed FAR region
    (coverage 0) always reads far (robust; a pit gives a clean far return),
    while a raised bump reads near ONLY where the PSF-integrated coverage is
    high. A thin 1-cell-wide bump line collects far on both flanks -> low
    integrated coverage -> washes toward far; a thick bump mass stays near."""
    if not (0.0 <= u < 1.0 and 0.0 <= v < 1.0):
        return 0.0                              # background moat
    c = int(u * 4.0)
    r = int(v * 4.0)
    if c > 3:
        c = 3
    if r > 3:
        r = 3
    if not grid[r][c]:
        return 0.0                              # flat cell
    if (r, c) in _ANCHOR_CELLS:
        return 1.0                              # solid registration pad
    fu = u * 4.0 - c
    fv = v * 4.0 - r
    m = (1.0 - P.bump_fill) * 0.5               # moat margin each side
    if fu < m or fu > 1.0 - m or fv < m or fv > 1.0 - m:
        return 0.0                              # moat gap around the pad
    return 1.0


def render(grid, ox, oy, fill, tu, tv, noise, rng):
    """Return (mm8[8][8], valid8[8][8]). ox,oy = tag origin in zone coords.

    Each zone reading is the PSF-weighted (Gaussian, sigma in zone units)
    integral of the height field over a footprint spanning ~+/-2.2 sigma zones,
    so an isolated sub-PSF feature loses contrast (the resolution limit)."""
    mm = [[0.0] * 8 for _ in range(8)]
    va = [[True] * 8 for _ in range(8)]
    taps = P.psf_taps
    sigma = P.psf_sigma
    span = 2.2 * sigma                     # half-width of the sampling footprint
    # precompute tap offsets + gaussian weights (1-D, separable)
    offs = []
    ws = []
    for t in range(taps):
        o = -span + (2.0 * span) * (t + 0.5) / taps
        offs.append(o)
        ws.append(math.exp(-0.5 * (o / sigma) ** 2))
    far = P.near_mm + P.step_mm
    for r in range(8):
        for c in range(8):
            cov = 0.0
            wsum = 0.0
            tiltsum = 0.0
            for ti, dor in enumerate(offs):
                zr = r + 0.5 + dor
                wr = ws[ti]
                for tj, doc in enumerate(offs):
                    zc = c + 0.5 + doc
                    w = wr * ws[tj]
                    u = (zc - ox) / fill
                    v = (zr - oy) / fill
                    cov += w * _coverage(u, v, grid)
                    tiltsum += w * (tu * (u - 0.5) + tv * (v - 0.5))
                    wsum += w
            cov /= wsum
            # depth = far minus the PSF-integrated bump coverage * physical step
            mm[r][c] = far - P.step_mm * cov + tiltsum / wsum + rng.gauss(0.0, noise)
    return mm, va


# ---------------------------------------------------------------------------
# k-means(k=2) threshold with min/max init, matching the firmware.
# ---------------------------------------------------------------------------
def _kmeans_thr(depths):
    lo = min(depths)
    hi = max(depths)
    c1 = float(lo)
    c2 = float(hi)
    for _ in range(16):
        s1 = s2 = 0.0
        n1 = n2 = 0
        for v in depths:
            if abs(v - c1) <= abs(v - c2):
                s1 += v
                n1 += 1
            else:
                s2 += v
                n2 += 1
        if n1 == 0 or n2 == 0:
            break
        nc1 = s1 / n1
        nc2 = s2 / n2
        if abs(nc1 - c1) < 0.5 and abs(nc2 - c2) < 0.5:
            c1, c2 = nc1, nc2
            break
        c1, c2 = nc1, nc2
    if c1 > c2:
        c1, c2 = c2, c1
    return c1, c2


# ---------------------------------------------------------------------------
# Faithful re-implementation of processFrameCV (anchor mode, pinned BR guard)
# + decodeAnchorBits. Single frame. Returns a dict describing the outcome.
#   result['kind'] in {'reject', 'clean', 'corrected', 'double', 'nogate'}
#   result['id']   decoded id (or None)
# ---------------------------------------------------------------------------
CU = (0.125, 0.875, 0.125, 0.875)   # 0=TL 1=TR 2=BL 3=BR (u = col)
CV = (0.125, 0.125, 0.875, 0.875)   # (v = row)


def decode_frame(mm, va):
    # 1. valid depths + median inlier filter (+/-25mm)
    alld = [mm[r][c] for r in range(8) for c in range(8) if va[r][c] and mm[r][c] > 0]
    if len(alld) < CV_MIN_VALID_ZONES:
        return {'kind': 'reject', 'id': None, 'why': 'few valid'}
    sd = sorted(alld)
    med = sd[len(sd) // 2]
    inlier = [[va[r][c] and mm[r][c] > 0 and abs(mm[r][c] - med) <= 25.0
               for c in range(8)] for r in range(8)]
    depths = [mm[r][c] for r in range(8) for c in range(8) if inlier[r][c]]
    if len(depths) < CV_MIN_VALID_ZONES:
        return {'kind': 'reject', 'id': None, 'why': 'few inlier'}

    # 2. k-means threshold
    c1, c2 = _kmeans_thr(depths)
    near_mm = round(c1)
    far_mm = round(c2)
    sep = far_mm - near_mm
    # 3. separation gate
    if sep < CV_SEP_MIN_MM:
        return {'kind': 'reject', 'id': None, 'why': 'sep small'}
    if sep > CV_SEP_MAX_MM:
        return {'kind': 'reject', 'id': None, 'why': 'sep large'}

    thr = (c1 + c2) * 0.5
    # 4. classify near, deadband +/-2mm
    near = [[False] * 8 for _ in range(8)]
    for r in range(8):
        for c in range(8):
            if not inlier[r][c]:
                continue
            v = mm[r][c]
            if abs(v - thr) < DEADBAND_MM:
                continue                    # ambiguous, excluded
            if v <= thr:
                near[r][c] = True
    amb = [[inlier[r][c] and abs(mm[r][c] - thr) < DEADBAND_MM for c in range(8)]
           for r in range(8)]

    # 5. bbox of NEAR
    rs = [r for r in range(8) for c in range(8) if near[r][c]]
    cs = [c for r in range(8) for c in range(8) if near[r][c]]
    if not rs:
        return {'kind': 'reject', 'id': None, 'why': 'no near'}
    bMinR, bMaxR = min(rs), max(rs)
    bMinC, bMaxC = min(cs), max(cs)
    bboxH = bMaxR - bMinR + 1
    bboxW = bMaxC - bMinC + 1
    if bboxH < CV_BBOX_MIN or bboxW < CV_BBOX_MIN:
        return {'kind': 'reject', 'id': None, 'why': 'bbox small'}
    if bboxH > CV_BBOX_MAX or bboxW > CV_BBOX_MAX:
        return {'kind': 'reject', 'id': None, 'why': 'bbox large'}

    def pad(lo, hi, want):
        mid = (lo + hi) // 2
        nlo = mid - (want - 1) // 2
        nhi = nlo + want - 1
        if nlo < 0:
            nlo, nhi = 0, want - 1
        if nhi > 7:
            nhi, nlo = 7, 8 - want
        return nlo, nhi
    if bboxH < CV_BBOX_PAD_TO:
        bMinR, bMaxR = pad(bMinR, bMaxR, CV_BBOX_PAD_TO)
    if bboxW < CV_BBOX_PAD_TO:
        bMinC, bMaxC = pad(bMinC, bMaxC, CV_BBOX_PAD_TO)

    # 6. graded weighted ring anchor centroids at all 4 bbox corners
    cornR = (bMinR, bMinR, bMaxR, bMaxR)   # TL TR BL BR
    cornC = (bMinC, bMaxC, bMinC, bMaxC)
    cI = [0.0] * 4
    cJ = [0.0] * 4
    wsum = [0.0] * 4
    for k in range(4):
        si = sj = wtot = 0.0
        for r in range(8):
            for c in range(8):
                if not near[r][c]:
                    continue
                dr = abs(r - cornR[k])
                dc = abs(c - cornC[k])
                d = dr if dr > dc else dc
                w = 3.0 if d <= 1.0 else 2.0 if d <= 2.0 else 1.0 if d <= 3.0 else 0.0
                if w <= 0.0:
                    continue
                si += w * (r + 0.5)
                sj += w * (c + 0.5)
                wtot += w
        wsum[k] = wtot
        cI[k] = si / wtot if wtot > 0 else cornR[k] + 0.5
        cJ[k] = sj / wtot if wtot > 0 else cornC[k] + 0.5

    # guard PINNED to BR (miss = 3); anchors are the other 3 corners
    miss = 3
    P3 = [k for k in range(4) if k != miss and wsum[k] > 0.0]
    if len(P3) < 3:
        return {'kind': 'reject', 'id': None, 'why': 'anchors not found'}
    P3 = P3[:3]

    # affine: (row i, col j) centroid -> unit u,v
    zi0, zj0 = cI[P3[0]], cJ[P3[0]]
    zi1, zj1 = cI[P3[1]], cJ[P3[1]]
    zi2, zj2 = cI[P3[2]], cJ[P3[2]]
    det = (zi1 - zi0) * (zj2 - zj0) - (zi2 - zi0) * (zj1 - zj0)
    if abs(det) < 1e-3:
        return {'kind': 'reject', 'id': None, 'why': 'degenerate'}

    def solve(f0, f1, f2):
        a = ((f1 - f0) * (zj2 - zj0) - (f2 - f0) * (zj1 - zj0)) / det
        b = ((zi1 - zi0) * (f2 - f0) - (zi2 - zi0) * (f1 - f0)) / det
        cc = f0 - a * zi0 - b * zj0
        return a, b, cc
    ua, ub, uc = solve(CU[P3[0]], CU[P3[1]], CU[P3[2]])
    va_, vb_, vc = solve(CV[P3[0]], CV[P3[1]], CV[P3[2]])

    # 7. sample each zone center through affine -> cell, count near
    cellNear = [0] * 16
    cellTot = [0] * 16
    for r in range(8):
        for c in range(8):
            if not inlier[r][c] or amb[r][c]:
                continue
            zi = r + 0.5
            zj = c + 0.5
            u = ua * zi + ub * zj + uc
            v = va_ * zi + vb_ * zj + vc
            if u < 0.0 or u >= 1.0 or v < 0.0 or v >= 1.0:
                continue
            cc2 = int(u * 4.0)
            cr2 = int(v * 4.0)
            if cc2 > 3:
                cc2 = 3
            if cr2 > 3:
                cr2 = 3
            i = cr2 * 4 + cc2
            cellTot[i] += 1
            if near[r][c]:
                cellNear[i] += 1

    # 8. per-cell majority -> bits16, count filled
    bits16 = [0] * 16
    nFilled = 0
    nearCells = 0
    for i in range(16):
        if cellTot[i] == 0:
            bits16[i] = 0
            continue
        nFilled += 1
        isNear = (cellNear[i] * 2 > cellTot[i])
        bits16[i] = 1 if isNear else 0
        if isNear:
            nearCells += 1
    if nFilled < MIN_VALID_CELLS:
        return {'kind': 'reject', 'id': None, 'why': 'nFilled<12'}
    if nearCells * 100 < MIN_NEAR_DENSITY_PCT * nFilled:
        return {'kind': 'reject', 'id': None, 'why': 'density'}

    # 9. SECDED decode at pinned rotation (rot 0). Anchor gate: 3 corners near.
    grid = [[bits16[r * 4 + c] for c in range(4)] for r in range(4)]
    if not (grid[0][0] and grid[0][3] and grid[3][0]):
        return {'kind': 'nogate', 'id': None, 'bits': grid, 'why': 'anchor gate'}
    cw = [grid[r][c] for (r, c) in CELL_ORDER]
    idv, status = secded_decode(cw)
    if status == 'double':
        return {'kind': 'double', 'id': None, 'bits': grid}
    if status == 'ok':
        return {'kind': 'clean', 'id': idv, 'bits': grid}
    return {'kind': 'corrected', 'id': idv, 'bits': grid}


# ---------------------------------------------------------------------------
# One randomized trial for a tag id. Returns the decode_frame result dict.
# ---------------------------------------------------------------------------
def _bilinear(mm, zr, zc):
    """Sample the 8x8 depth field at continuous (zr row, zc col), clamped."""
    if zr < 0:
        zr = 0.0
    if zr > 7:
        zr = 7.0
    if zc < 0:
        zc = 0.0
    if zc > 7:
        zc = 7.0
    r0 = int(zr)
    c0 = int(zc)
    r1 = min(r0 + 1, 7)
    c1 = min(c0 + 1, 7)
    fr = zr - r0
    fc = zc - c0
    return (mm[r0][c0] * (1 - fr) * (1 - fc) + mm[r0][c1] * (1 - fr) * fc +
            mm[r1][c0] * fr * (1 - fc) + mm[r1][c1] * fr * fc)


# Cell-center sub-offsets (in unit-cell fractions) the firmware averages over.
_SUB = (-0.18, 0.0, 0.18)


def frame_votes(mm, ox, oy, fill, jr, jc, js):
    """One frame -> (cellNear[16], cellTot[16], thr) or (None, why, None).

    Firmware step 7: each ZONE center maps through the affine to unit (u,v);
    zones with (u,v) in [0,1) vote for their cell (background zones fall outside
    and are excluded), a zone is NEAR if depth <= thr (deadband skipped). On real
    hardware the graded-ring + calibration recovers the pose correctly for
    essentially every tag (per HRScanEngine.cpp), so we use the KNOWN pose plus a
    small residual jitter (jr,jc translation zones; js isotropic scale error)
    rather than re-deriving a skewed affine from the noisy near-grid."""
    alld = [mm[r][c] for r in range(8) for c in range(8)]
    sd = sorted(alld)
    med = sd[len(sd) // 2]
    depths = [x for x in alld if abs(x - med) <= 25.0]
    if len(depths) < CV_MIN_VALID_ZONES:
        return None, 'few inlier', None
    c1, c2 = _kmeans_thr(depths)
    if (c2 - c1) < CV_SEP_MIN_MM:
        return None, 'sep small', None
    if (c2 - c1) > CV_SEP_MAX_MM:
        return None, 'sep large', None
    thr = (c1 + c2) * 0.5
    cx = ox + 0.5 * fill
    cy = oy + 0.5 * fill
    cellNear = [0] * 16
    cellTot = [0] * 16
    inv_fill = 1.0 / (fill * js)
    for r in range(8):
        for c in range(8):
            u = ((c + 0.5) - jc - cx) * inv_fill + 0.5
            v = ((r + 0.5) - jr - cy) * inv_fill + 0.5
            if u < 0.0 or u >= 1.0 or v < 0.0 or v >= 1.0:
                continue
            val = mm[r][c]
            if abs(val - thr) < DEADBAND_MM:
                continue
            i = int(v * 4.0) * 4 + int(u * 4.0)
            cellTot[i] += 1
            if val <= thr:
                cellNear[i] += 1
    return cellNear, cellTot, thr


def decode_votes(cellNear, cellTot):
    """Windowed per-cell majority -> SECDED result dict (firmware step 8b/9)."""
    bits = [[0] * 4 for _ in range(4)]
    nFilled = 0
    for i in range(16):
        if cellTot[i] == 0:
            continue
        nFilled += 1
        if cellNear[i] * 2 > cellTot[i]:
            bits[i // 4][i % 4] = 1
    if nFilled < MIN_VALID_CELLS:
        return {'kind': 'reject', 'id': None, 'why': 'nFilled<12'}
    if not (bits[0][0] and bits[0][3] and bits[3][0]):
        return {'kind': 'nogate', 'id': None, 'bits': bits, 'why': 'anchor gate'}
    cw = [bits[rr][cc] for (rr, cc) in CELL_ORDER]
    idv, status = secded_decode(cw)
    if status == 'double':
        return {'kind': 'double', 'id': None, 'bits': bits}
    if status == 'ok':
        return {'kind': 'clean', 'id': idv, 'bits': bits}
    return {'kind': 'corrected', 'id': idv, 'bits': bits}


def decode_pose(mm, ox, oy, fill, jr, jc, js):
    """Single-frame convenience wrapper (used by sample_ideal_bits)."""
    cn, ct, _ = frame_votes(mm, ox, oy, fill, jr, jc, js)
    if cn is None:
        return {'kind': 'reject', 'id': None, 'why': ct}
    return decode_votes(cn, ct)


def trial(idv, rng, grid=None):
    """One READ = a `P.frames`-frame vote window (firmware kCvVoteHistory).

    Each read picks a base alignment (the badge is held roughly steady for the
    window), then renders `frames` frames with independent per-frame sensor noise
    and small residual pose jitter, accumulating per-cell near/total votes across
    the window before thresholding -- exactly as the firmware sums votes over its
    sliding history. This smooths random noise (robust tags read very clean) while
    leaving systematic per-pose washout (the fragile-cell failures) intact."""
    if grid is None:
        grid = grid_for(idv)
    fill = rng.uniform(P.fill_lo, P.fill_hi)
    base_off = (8.0 - fill) / 2.0
    ox = base_off + rng.uniform(-P.off_jit, P.off_jit)
    oy = base_off + rng.uniform(-P.off_jit, P.off_jit)
    tmag = rng.uniform(0.0, P.tilt_max)
    ang = rng.uniform(0, 2 * math.pi)
    tu = tmag * math.cos(ang)
    tv = tmag * math.sin(ang)
    # persistent (per-read) residual pose error the calibrated affine can't remove
    jr0 = rng.gauss(0.0, P.pose_jit)
    jc0 = rng.gauss(0.0, P.pose_jit)
    js = 1.0 + rng.gauss(0.0, P.pose_scale_jit)
    cellNear = [0] * 16
    cellTot = [0] * 16
    any_frame = False
    for _ in range(P.frames):
        noise = rng.uniform(P.noise_lo, P.noise_hi)
        mm, _va = render(grid, ox, oy, fill, tu, tv, noise, rng)
        # small per-frame pose flicker around the read's residual offset
        jr = jr0 + rng.gauss(0.0, P.pose_flicker)
        jc = jc0 + rng.gauss(0.0, P.pose_flicker)
        cn, ct, _thr = frame_votes(mm, ox, oy, fill, jr, jc, js)
        if cn is None:
            continue
        any_frame = True
        for i in range(16):
            cellNear[i] += cn[i]
            cellTot[i] += ct[i]
    if not any_frame:
        return {'kind': 'reject', 'id': None, 'why': 'all frames gated'}
    return decode_votes(cellNear, cellTot)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
def reliability(idv, n=300, seed=1234):
    """Fraction of trials that CLEANLY self-decode (Ok + syndrome 0 + own id)."""
    rng = random.Random(seed * 131 + idv)
    grid = grid_for(idv)
    clean = 0
    for _ in range(n):
        res = trial(idv, rng, grid)
        if res['kind'] == 'clean' and res['id'] == idv:
            clean += 1
    return clean / n


def breakdown(idv, n=300, seed=1234):
    """Detailed outcome tally for one id (for diagnostics)."""
    rng = random.Random(seed * 131 + idv)
    grid = grid_for(idv)
    tally = {'clean_self': 0, 'clean_wrong': 0, 'corrected_self': 0,
             'corrected_wrong': 0, 'double': 0, 'reject': 0, 'nogate': 0}
    for _ in range(n):
        res = trial(idv, rng, grid)
        k = res['kind']
        if k == 'clean':
            tally['clean_self' if res['id'] == idv else 'clean_wrong'] += 1
        elif k == 'corrected':
            tally['corrected_self' if res['id'] == idv else 'corrected_wrong'] += 1
        elif k in ('double', 'reject', 'nogate'):
            tally[k] += 1
    return tally


def sample_ideal_bits(idv):
    """The 4x4 bit grid a well-aligned, low-noise frame samples (debug signal)."""
    rng = random.Random(9)
    grid = grid_for(idv)
    fill = 7.0
    off = (8.0 - fill) / 2.0
    mm, va = render(grid, off, off, fill, 0.0, 0.0, 0.15, rng)
    return decode_pose(mm, off, off, fill, 0.0, 0.0, 1.0)


def rank_all(n=300, seed=1234):
    return sorted(((i, reliability(i, n, seed)) for i in range(1, 128)),
                  key=lambda t: t[1], reverse=True)


# ---------------------------------------------------------------------------
# main(): calibration table + full ranking
# ---------------------------------------------------------------------------
CAL = [
    (101, 'FAIL  (->37, never clean)'),
    (43,  'FAIL  (BadCRC)'),
    (47,  'RELIABLE'),
    (55,  'RELIABLE'),
    (88,  'RELIABLE'),
    (37,  'RELIABLE'),
    (64,  'RELIABLE'),
    (73,  'RELIABLE'),
    (16,  'MARGINAL (slow lock)'),
]

HIGH_BAR = 0.60
MED_BAR = 0.45


def main():
    n = 400
    print("=" * 66)
    print("HR-code scanner reliability simulator")
    print("model: FILL %.1f-%.1f  off+/-%.2f  tilt<=%.2f  noise %.1f-%.1f mm  "
          "psf_sigma=%.2fz  step=%dmm"
          % (P.fill_lo, P.fill_hi, P.off_jit, P.tilt_max, P.noise_lo,
             P.noise_hi, P.psf_sigma, int(P.step_mm)))
    print("trials/id: %d   clean = Ok + syndrome0 + own id" % n)
    print("=" * 66)

    print("\nCALIBRATION (known hardware results):")
    print("  %-4s %-8s %-9s  %s" % ("id", "score", "ideal", "hardware"))
    for idv, hw in CAL:
        sc = reliability(idv, n)
        ideal = sample_ideal_bits(idv)
        ib = ideal.get('id')
        itag = "%s%s" % (ideal['kind'][:4],
                         ("=%d" % ib) if ib is not None else "")
        print("  %-4d %6.3f   %-9s  %s" % (idv, sc, itag, hw))

    # show 101's sampled bits explicitly (target 1001/1001/0001/1001 -> 37)
    ideal101 = sample_ideal_bits(101)
    if 'bits' in ideal101:
        rows = ["".join(str(b) for b in row) for row in ideal101['bits']]
        print("\n  id 101 ideal sampled bits: %s  -> %s id=%s (target: "
              "1001/1001/0001/1001 -> 37)"
              % ("/".join(rows), ideal101['kind'], ideal101.get('id')))

    print("\nFULL RANKING (id: score), 1..127 by reliability:")
    ranked = rank_all(n)
    for i in range(0, len(ranked), 6):
        row = ranked[i:i + 6]
        print("  " + "   ".join("%3d:%.3f" % (idv, sc) for idv, sc in row))

    high = [i for i, s in ranked if s >= HIGH_BAR]
    med = [i for i, s in ranked if s >= MED_BAR]
    print("\nreliable IDs (score >= %.2f, HIGH bar): %d" % (HIGH_BAR, len(high)))
    print("  " + ", ".join(str(i) for i in sorted(high)))
    print("\nreliable IDs (score >= %.2f, MED bar):  %d" % (MED_BAR, len(med)))
    print("  " + ", ".join(str(i) for i in sorted(med)))

    print("\n" + "-" * 66)
    print("CALIBRATION FIT / KNOWN LIMITATION (honest):")
    print("  Correctly ranked: 43 clearly LOW; 64/47/88/73 HIGH (55 lands mid).")
    print("  MIS-RANKED: id 37 (hw RELIABLE) scores LOW, and id 101 (hw FAIL)")
    print("  scores MID. Root cause: 37 and 101 are geometrically near-identical")
    print("  in fragility (both have 5 'thin' 1-cell-wide near structures), so no")
    print("  bit-pattern-only model separates them -- their differing hardware")
    print("  verdicts come from effects outside the idealized geometry (per-zone")
    print("  sensor response, print registration, calibration state). This matches")
    print("  the hardware team's own note that the isolated-cell metric fails on")
    print("  the newer 37/101 data. Treat MID-band scores as 'verify on hardware'.")


if __name__ == "__main__":
    main()
