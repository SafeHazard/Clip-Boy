#!/usr/bin/env python3
"""
hm_qr_3mf_2color_topcap.py

Bambu Studio / AMS-friendly 2-color 3MF with:
- HM heightmap top surface (flat per HM cell)
- QR is COLOR-ONLY and only on the TOP CAP layers (minimizes filament swaps)

Two objects:
  Object 1 (white): solid up to top_height, but in QR-black regions it stops at (top_height - cap)
  Object 2 (black): only a thin cap in QR-black regions from (top_height - cap) to top_height

This avoids per-layer color swaps and avoids mesh overlap.

Default output is the ANCHOR tag (A3-D13, SECDED(12,7)) at the standard badge
geometry (60mm, 14mm step, 5mm base, 5mm moat border, no bump pre-mirror,
256px QR). So the common case is just:
    python hm_codegen.py --id 73 --qr yourQR.png --out 73.3mf
Use --legacy for the old orientation-marker 4x4; override any geometry flag as needed.

Requires: Pillow (pip install pillow)
"""

import argparse
import os
import zipfile
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from PIL import Image


# ----------------------------
# HM mapping
# ----------------------------
GRID_N = 4

ORIENT = {(0, 0): 1, (0, 1): 1, (1, 0): 1, (1, 1): 0}

PAYLOAD_CELLS = [
    (0, 2), (0, 3),
    (1, 2), (1, 3),
    (2, 0), (2, 1), (2, 2), (2, 3),
    (3, 0), (3, 1), (3, 2), (3, 3),
]
ID_CELLS = PAYLOAD_CELLS[:8]
CRC_CELLS = PAYLOAD_CELLS[8:12]

# IDs whose rotated forms can decode as another valid ID under the current
# orientation-marker + CRC scheme. Generation rejects these by default.
AMBIGUOUS_ID_PEERS = {
    11: 146,
    59: 82,
    82: 59,
    91: 158,
    94: 107,
    107: 94,
    146: 11,
    158: 91,
}


def crc4_nibble_fold(id8: int) -> int:
    return (id8 ^ (id8 >> 4)) & 0xF


# ── SECDED(12,7) for the anchor spec (DC34-155) ─────────────────────────────
# The anchor layout has 13 data cells, but (3,3) MUST stay 0 as a rotation
# guard (a 0 in the BR corner is what makes the 3 wrong rotations fail the
# 3-anchor gate in the decoder; otherwise ECC's large accept region would
# false-accept a wrong-rotation read and unlock the wrong id). So the ECC lives
# in the OTHER 12 cells: extended Hamming(11,7) + 1 overall parity = SECDED(12,7),
# distance 4. 7-bit id covers 0..127 (collectibles are 1..100). Corrects any
# single-cell flip (the ~1-cell/frame VL53L5CX flicker) so the read locks;
# detects (rejects) any double flip so a corrupted read never mis-unlocks.
# Codeword bit order into the 12 row-major data cells (all EXCEPT the (3,3)
# guard): cw[0..10] = Hamming positions 1..11, cw[11] = overall parity.
# Reference + verification: scripts/hr_spec/secded.py (0 miscorrects /
# 0 mis-decodes, all 128 IDs, single + double flips).
SECDED_PPOS = (1, 2, 4, 8)                    # parity positions (powers of two)
SECDED_DPOS = (3, 5, 6, 7, 9, 10, 11)         # the 7 data positions in 1..11


def secded_encode(id7: int) -> List[int]:
    """id7 (0..127) -> 12 codeword bits (index 0..11) for the 12 non-guard cells."""
    if not (0 <= id7 <= 127):
        raise ValueError("anchor id must be 0..127 (7-bit SECDED)")
    h = [0] * 12                              # h[1..11] used
    for i in range(7):
        h[SECDED_DPOS[i]] = (id7 >> i) & 1
    for p in SECDED_PPOS:                      # even parity over covered bits
        s = 0
        for j in range(1, 12):
            if j != p and (j & p):
                s ^= h[j]
        h[p] = s
    overall = 0
    for j in range(1, 12):
        overall ^= h[j]
    return [h[j] for j in range(1, 12)] + [overall]


def validate_id(id8: int, allow_ambiguous: bool = False) -> None:
    if not (0 <= id8 <= 255):
        raise ValueError("id must be 0..255")

    peer = AMBIGUOUS_ID_PEERS.get(id8)
    if peer is None:
        return

    msg = (
        f"id {id8} is ambiguous: a rotated tag can impersonate id {peer}. "
        "Avoid these IDs for production tags."
    )
    if allow_ambiguous:
        print(f"WARNING: {msg}")
        return
    raise ValueError(msg + " Re-run with --allow-ambiguous to override.")


def build_bit_grid(id8: int, allow_ambiguous: bool = False) -> List[List[int]]:
    validate_id(id8, allow_ambiguous=allow_ambiguous)

    g = [[0]*GRID_N for _ in range(GRID_N)]
    for (r, c), v in ORIENT.items():
        g[r][c] = v

    for i, (r, c) in enumerate(ID_CELLS):
        g[r][c] = (id8 >> i) & 1

    crc = crc4_nibble_fold(id8)
    for i, (r, c) in enumerate(CRC_CELLS):
        g[r][c] = (crc >> i) & 1

    return g


# ── Anchor / fiducial layout "A3-D13" (HR spec v2, DC34-155) ────────────────
# 3 asymmetric corner anchors (always-near) give the decoder an affine pose
# (position/scale/rotation) and a relative-depth reference; the other 13 cells
# carry data, sampled at pose-corrected cell centers. Validated in sim to read
# far more robustly than the orientation-marker layout on the VL53L5CX 8x8
# (see scripts/hr_spec/). Anchors at TL, TR, BL — the missing BR corner fixes
# rotation (QR-finder style). Data cells row-major (minus anchors): the first 12
# hold a SECDED(12,7) codeword over the 7 id bits (11 Hamming positions + 1
# overall parity); cell (3,3) stays 0 as the rotation GUARD (see secded_encode).
# SECDED corrects the ~1-cell per-frame flicker so the read locks; a double flip
# is rejected, never mis-unlocked. Do NOT print with --bump_margin (it dilutes
# near/far separation on this layout); print --no_mirror_x and scan bump-side up.
ANCHOR_CELLS = [(0, 0), (0, 3), (3, 0)]
GUARD_CELL = (3, 3)                                        # must stay 0 (rotation guard)


def build_anchor_grid(id7: int) -> List[List[int]]:
    if not (0 <= id7 <= 127):
        raise ValueError("anchor id must be 0..127 (7-bit SECDED)")
    g = [[0] * GRID_N for _ in range(GRID_N)]
    for (r, c) in ANCHOR_CELLS:
        g[r][c] = 1
    # 12 codeword cells = all cells except the 3 anchors AND the (3,3) guard.
    data_cells = [(r, c) for r in range(GRID_N) for c in range(GRID_N)
                  if (r, c) not in ANCHOR_CELLS and (r, c) != GUARD_CELL]
    seq = secded_encode(id7)                               # 12 codeword bits
    for (r, c), b in zip(data_cells, seq):                 # (3,3) untouched -> 0
        g[r][c] = b
    return g


# ----------------------------
# Mesh helpers
# ----------------------------
@dataclass
class Mesh:
    v: List[Tuple[float, float, float]]
    f: List[Tuple[int, int, int]]
    _vmap: Dict[Tuple[float, float, float], int] = field(default_factory=dict, repr=False)
    _fset: Set[Tuple[int, int, int]] = field(default_factory=set, repr=False)


def add_v(mesh: Mesh, p: Tuple[float, float, float]) -> int:
    key = (round(p[0], 9), round(p[1], 9), round(p[2], 9))
    i = mesh._vmap.get(key)
    if i is not None:
        return i
    mesh.v.append(key)
    i = len(mesh.v) - 1
    mesh._vmap[key] = i
    return i


def add_tri(mesh: Mesh, a: int, b: int, c: int) -> None:
    if a == b or b == c or a == c:
        return
    k = tuple(sorted((a, b, c)))
    if k in mesh._fset:
        return
    mesh._fset.add(k)
    mesh.f.append((a, b, c))


def add_quad(mesh: Mesh, p00, p10, p11, p01) -> None:
    i00 = add_v(mesh, p00)
    i10 = add_v(mesh, p10)
    i11 = add_v(mesh, p11)
    i01 = add_v(mesh, p01)
    add_tri(mesh, i00, i10, i11)
    add_tri(mesh, i00, i11, i01)


def build_union_mesh_from_intervals(
    intervals: List[List[Optional[Tuple[float, float]]]],
    px_w: float,
    px_h: float,
    ambiguity_mode: str = "parity",
    eps: float = 1e-9,
) -> Mesh:
    """
    Build one watertight boundary mesh for the union of all occupied interval voxels.
    intervals[y][x] is None or (z0, z1).
    """
    h = len(intervals)
    w = len(intervals[0]) if h else 0
    mesh = Mesh(v=[], f=[])
    if h == 0 or w == 0:
        return mesh

    zvals = set()
    for y in range(h):
        for x in range(w):
            iv = intervals[y][x]
            if iv is None:
                continue
            z0, z1 = iv
            if z1 - z0 > eps:
                zvals.add(round(z0, 9))
                zvals.add(round(z1, 9))

    z_levels = sorted(zvals)
    if len(z_levels) < 2:
        return mesh

    nz = len(z_levels) - 1
    occ = [[[False for _ in range(nz)] for _ in range(w)] for _ in range(h)]

    for y in range(h):
        for x in range(w):
            iv = intervals[y][x]
            if iv is None:
                continue
            z0, z1 = iv
            if z1 - z0 <= eps:
                continue
            for k in range(nz):
                zl = z_levels[k]
                zh = z_levels[k + 1]
                if zh - zl <= eps:
                    continue
                if (zl >= z0 - eps) and (zh <= z1 + eps):
                    occ[y][x][k] = True

    # Resolve 2D checkerboard ambiguities per z-slab; they create non-manifold
    # edge-touching components (4 faces around one geometric edge).
    # ambiguity_mode == "ignore" leaves the geometry as-is (4-faces-on-edge —
    # slicers handle this fine and we avoid adding spurious sub-cell material
    # that would corrupt a coarse LiDAR sample of the heightmap).
    if ambiguity_mode != "ignore":
        for k in range(nz):
            for y in range(h - 1):
                for x in range(w - 1):
                    a = occ[y][x][k]
                    b = occ[y][x + 1][k]
                    c = occ[y + 1][x][k]
                    d = occ[y + 1][x + 1][k]
                    if a and d and (not b) and (not c):
                        if ambiguity_mode == "fill_both":
                            occ[y][x + 1][k] = True
                            occ[y + 1][x][k] = True
                        elif ((x + y) & 1) == 0:
                            occ[y][x + 1][k] = True
                        else:
                            occ[y + 1][x][k] = True
                    elif b and c and (not a) and (not d):
                        if ambiguity_mode == "fill_both":
                            occ[y][x][k] = True
                            occ[y + 1][x + 1][k] = True
                        elif ((x + y) & 1) == 0:
                            occ[y][x][k] = True
                        else:
                            occ[y + 1][x + 1][k] = True

    def is_occ(y: int, x: int, k: int) -> bool:
        if y < 0 or y >= h or x < 0 or x >= w or k < 0 or k >= nz:
            return False
        return occ[y][x][k]

    for y in range(h):
        y0 = y * px_h
        y1 = (y + 1) * px_h
        for x in range(w):
            x0 = x * px_w
            x1 = (x + 1) * px_w
            for k in range(nz):
                if not occ[y][x][k]:
                    continue
                z0 = z_levels[k]
                z1 = z_levels[k + 1]

                if not is_occ(y, x, k - 1):  # -Z
                    add_quad(mesh, (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0))
                if not is_occ(y, x, k + 1):  # +Z
                    add_quad(mesh, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))
                if not is_occ(y - 1, x, k):  # -Y
                    add_quad(mesh, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))
                if not is_occ(y + 1, x, k):  # +Y
                    add_quad(mesh, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1))
                if not is_occ(y, x - 1, k):  # -X
                    add_quad(mesh, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1))
                if not is_occ(y, x + 1, k):  # +X
                    add_quad(mesh, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))

    return mesh


# ----------------------------
# QR mask + height
# ----------------------------
def load_qr_mask_stretched(png_path: str, out_px: int, thresh: int, invert: bool,
                           smooth: bool = False) -> List[List[bool]]:
    """
    Loads the PNG and stretches it to out_px x out_px, returns
    mask[y][x] where True = "ink" regions (black by default).

    smooth=False (default): NEAREST resize. Right for QR codes where
                  module boundaries must stay sharp.
    smooth=True:  LANCZOS resize. Right for stylized text/logos where
                  smooth strokes matter more than module fidelity.
    """
    im = Image.open(png_path).convert("L")
    resample = Image.LANCZOS if smooth else Image.NEAREST
    im = im.resize((out_px, out_px), resample)

    px = im.load()
    mask = [[False]*out_px for _ in range(out_px)]
    for y in range(out_px):
        for x in range(out_px):
            is_black = px[x, y] < thresh
            mask[y][x] = (not is_black) if invert else is_black
    return mask


def hm_top_height_at_xy(bits: List[List[int]], x: float, y: float,
                        tag_size_mm: float, base_mm: float, step_mm: float,
                        bump_margin_mm: float = 0.0,
                        mirror_x: bool = True,
                        border_mm: float = 0.0,
                        border_recess_mm: float = 0.0,
                        taper_border: bool = False,
                        bump_height_mm: float = None) -> float:
    # bump_height_mm: how far a "1" cell rises above a "0" cell. Defaults to
    # step_mm (legacy: bump height == cell spacing). Decoupled so bumps can be
    # made taller for more LiDAR near/far separation without enlarging the tag.
    if bump_height_mm is None:
        bump_height_mm = step_mm
    # If a recessed border is configured, coords outside the central
    # tag_size_mm area sit in the moat: height = base - recess. The moat
    # gives the LiDAR a deterministic FAR cluster regardless of how close
    # the user holds the badge, fixing the fills-FoV failure mode.
    if border_mm > 0.0:
        bx = x - border_mm
        by = y - border_mm
        in_tag = (0.0 <= bx < tag_size_mm) and (0.0 <= by < tag_size_mm)
        if not in_tag:
            moat = max(0.0, base_mm - border_recess_mm)
            if not taper_border:
                return moat
            # Keying border: full at the BOTTOM (max-Y edge -- the corner anchor
            # sits at its lower-left, guard at lower-right), left/right borders
            # taper from full (bottom) to zero at the top corners, and the TOP
            # border is removed. So the tag only drops into a stand one way up.
            if by < 0.0:                         # top edge (min-Y): no border
                return 0.0
            if by >= tag_size_mm:                # full-width bottom bar (max-Y)
                return moat
            side = border_mm * (by / tag_size_mm)   # side width, 0 (top) -> full (bottom)
            if (-side <= bx < 0.0) or (tag_size_mm <= bx < tag_size_mm + side):
                return moat
            return 0.0                           # tapered-away side region
        x = bx
        y = by
    cell = tag_size_mm / GRID_N
    c = min(GRID_N - 1, max(0, int(x / cell)))
    r = min(GRID_N - 1, max(0, int(y / cell)))
    # bump_margin_mm: force "low" within this distance of any cell boundary,
    # even if the cell's bit is 1. Creates a clean low-band gutter between
    # adjacent high cells so the LiDAR's bilinear sampler always reads "low"
    # at cell boundaries, regardless of sub-cell alignment jitter. Critical for
    # dense bit patterns (alternating rows like ID 55's 1 0 1 0) where adjacent
    # high/low transitions otherwise cause frame-to-frame bit flips.
    # NOTE: this gutter is a GEOMETRIC property of the cell grid, so it MUST be
    # computed from the actual (pre-mirror) column c. It used to run AFTER the
    # mirror_x reassignment below, which combined a mirrored column with the
    # un-mirrored x -> edge_dist went hugely negative for every pixel -> every
    # cell returned base -> ALL BUMPS VANISHED whenever mirror_x was on (the
    # default). Keep this block above the mirror.
    if bump_margin_mm > 0.0:
        x_in_cell = x - c * cell
        y_in_cell = y - r * cell
        edge_dist = min(x_in_cell, cell - x_in_cell, y_in_cell, cell - y_in_cell)
        if edge_dist < bump_margin_mm:
            return base_mm
    # mirror_x: pre-mirror the column index so the printed-and-flipped tag
    # presents canonical bits (not X-mirrored) to the LiDAR. The 3D printing
    # workflow inherently inverts X when the user flips the print to face
    # bumps-toward-sensor; pre-mirroring here cancels that flip. Default True
    # for badge production. Set False to get raw canonical bits in the .3mf
    # coordinate frame (useful for verifying script output).
    if mirror_x:
        c = GRID_N - 1 - c
    return base_mm + (bits[r][c] * bump_height_mm)


# ----------------------------
# Build white (bulk) + black (top cap) meshes
# ----------------------------
def build_topcap_meshes(
    bits: List[List[int]],
    qr_mask: List[List[bool]],
    tag_size_mm: float,
    base_mm: float,
    step_mm: float,
    cap_mm: float,
    seam_gap_mm: float,
    bump_margin_mm: float = 0.0,
    mirror_x: bool = True,
    qr_mirror_x: bool = True,
    border_mm: float = 0.0,
    border_recess_mm: float = 0.0,
    taper_border: bool = False,
    bump_height_mm: float = None,
) -> Tuple[Mesh, Mesh]:
    """
    For each QR raster pixel:
      - compute HM top height H
      - if ink (True): white interval [0, H-cap], black interval [H-cap+gap, H]
      - else:          white interval [0, H],     black none
    Then convert each interval field into a single watertight boundary mesh.
    """
    # If a recessed border is configured, expand the raster to cover the
    # full print (bumps area + border on all sides) and pad the QR ink
    # mask with no-ink moat pixels so the QR doesn't bleed onto the moat.
    if border_mm > 0.0:
        qr_h = len(qr_mask)
        qr_w = len(qr_mask[0]) if qr_h else 0
        if qr_w == 0 or qr_h == 0:
            raise ValueError("Empty QR mask")
        # Match raster density to the existing qr_w pixels-per-tag_size_mm
        px_per_mm = qr_w / tag_size_mm
        border_px = max(1, int(round(border_mm * px_per_mm)))
        total_px = qr_w + 2 * border_px
        new_mask = [[False] * total_px for _ in range(total_px)]
        for ry in range(qr_h):
            for rx in range(qr_w):
                new_mask[ry + border_px][rx + border_px] = qr_mask[ry][rx]
        qr_mask = new_mask
        total_size_mm = tag_size_mm + 2 * border_mm
    else:
        total_size_mm = tag_size_mm

    h = len(qr_mask)
    w = len(qr_mask[0]) if h else 0
    if w == 0 or h == 0:
        raise ValueError("Empty QR mask")

    white_intervals: List[List[Optional[Tuple[float, float]]]] = [[None] * w for _ in range(h)]
    black_intervals: List[List[Optional[Tuple[float, float]]]] = [[None] * w for _ in range(h)]

    px_w = total_size_mm / w
    px_h = total_size_mm / h

    for y in range(h):
        for x in range(w):
            # The QR/text is a HUMAN-read image; the bumps are a LiDAR-read depth
            # pattern. They need INDEPENDENT mirroring: `mirror_x` pre-mirrors the
            # BUMPS for the scan flip (off under --no_mirror_x for anchor tags,
            # whose raw-canonical bumps the sensor reads correctly), but the QR
            # must ALWAYS pre-mirror (qr_mirror_x, default True) to render the PNG
            # right side up on the printed top surface. Coupling them made the
            # text read BACKWARDS under --no_mirror_x.
            mx = (w - 1 - x) if qr_mirror_x else x
            ink = qr_mask[y][mx]
            xc = (x + 0.5) * px_w
            yc = (y + 0.5) * px_h
            H = hm_top_height_at_xy(bits, xc, yc, tag_size_mm, base_mm, step_mm,
                                    bump_margin_mm, mirror_x,
                                    border_mm, border_recess_mm, taper_border,
                                    bump_height_mm)

            # White plate is uniform full-height across all pixels (no
            # recess under ink); black cap sits ON TOP of the white for
            # ink pixels, separated by seam_gap_mm so the meshes don't
            # share a boundary face. Slicers handle the white/black
            # filament transition cleanly without an internal cavity.
            if H > 1e-12:
                white_intervals[y][x] = (0.0, H)
            if ink and cap_mm > 1e-12:
                z_black0 = H + max(0.0, seam_gap_mm)
                z_black1 = z_black0 + cap_mm
                black_intervals[y][x] = (z_black0, z_black1)

    # Both meshes MUST use the same ambiguity mode -- otherwise the black
    # cap extends past the white plate's notches at diagonal corners,
    # which slicers render as a visible X/Y offset of the cap relative
    # to the plate. We use "fill_both" everywhere: at sub-cell checker
    # corners the resulting bump-side bulge is at most one raster pixel
    # (~0.23 mm at qr_px=256), well below the LiDAR's ±15 mm depth
    # tolerance and far smaller than a single zone (~7 mm at scan
    # distance), so it's invisible to the bump decoder.
    white_mesh = build_union_mesh_from_intervals(white_intervals, px_w, px_h, ambiguity_mode="fill_both")
    black_mesh = build_union_mesh_from_intervals(black_intervals, px_w, px_h, ambiguity_mode="fill_both")
    return white_mesh, black_mesh


# ----------------------------
# 3MF writer (two objects)
# ----------------------------
def mesh_to_xml(mesh: Mesh) -> str:
    v_xml = "\n".join(f'<vertex x="{x:.6f}" y="{y:.6f}" z="{z:.6f}"/>' for (x, y, z) in mesh.v)
    t_xml = "\n".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for (a, b, c) in mesh.f)
    return f"""
      <mesh>
        <vertices>
          {v_xml}
        </vertices>
        <triangles>
          {t_xml}
        </triangles>
      </mesh>
    """


def build_model_xml(mesh_white: Mesh, mesh_black: Mesh) -> str:
    # Skip emitting the black object if it has no geometry (e.g. when the QR
    # mask is blank). Slicers warn about zero-volume objects otherwise.
    has_black = len(mesh_black.f) > 0
    objects_xml = f'''    <object id="1" type="model">
      {mesh_to_xml(mesh_white)}
    </object>'''
    items_xml = '<item objectid="1"/>'
    if has_black:
        objects_xml += f'''
    <object id="2" type="model">
      {mesh_to_xml(mesh_black)}
    </object>'''
        items_xml += '\n    <item objectid="2"/>'
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US"
  xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
  <resources>
{objects_xml}
  </resources>
  <build>
    {items_xml}
  </build>
</model>
'''


def write_3mf(out_path: str, model_xml: str, has_black: bool = True) -> None:
    content_types = '''<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>
'''
    rels_root = '''<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rel0"
    Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"
    Target="/3D/3dmodel.model"/>
</Relationships>
'''
    # Bambu Studio metadata: pin Object 1 (white) -> extruder 1, Object 2
    # (black cap) -> extruder 2. Without this, Bambu's auto-detection of
    # multi-part .3mfs is inconsistent -- some files get the multi-object
    # prompt and end up with Object 2 visible + assignable, others get
    # silently merged or hidden. Explicit per-object extruder mapping
    # forces Bambu to load both objects with the right filament.
    if has_black:
        model_settings = '''<?xml version="1.0" encoding="UTF-8"?>
<config>
  <object id="1">
    <metadata key="name" value="HM_white"/>
    <metadata key="extruder" value="1"/>
    <part id="1" subtype="normal_part">
      <metadata key="extruder" value="1"/>
    </part>
  </object>
  <object id="2">
    <metadata key="name" value="HM_black_cap"/>
    <metadata key="extruder" value="2"/>
    <part id="2" subtype="normal_part">
      <metadata key="extruder" value="2"/>
    </part>
  </object>
</config>
'''
    else:
        model_settings = '''<?xml version="1.0" encoding="UTF-8"?>
<config>
  <object id="1">
    <metadata key="name" value="HM_white"/>
    <metadata key="extruder" value="1"/>
    <part id="1" subtype="normal_part">
      <metadata key="extruder" value="1"/>
    </part>
  </object>
</config>
'''

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels_root)
        z.writestr("3D/3dmodel.model", model_xml)
        z.writestr("Metadata/model_settings.config", model_settings)


def main():
    ap = argparse.ArgumentParser(description="2-color HM tag with QR only on top cap (min AMS swaps).")
    ap.add_argument("--id", type=int, required=True, help="ID (anchor mode 0..127; legacy 0..255)")
    ap.add_argument("--qr", type=str, required=True, help="QR PNG path")
    ap.add_argument("--out", type=str, required=True, help="Output .3mf")
    ap.add_argument("--size", type=float, default=60.0, help="Tag size mm (default 60)")
    ap.add_argument("--base", type=float, default=5.0, help="Base thickness mm (default 5)")
    ap.add_argument("--step", type=float, default=14.0, help="HM step mm / cell spacing (default 14)")
    ap.add_argument("--bump_height", type=float, default=None,
                    help="Bump cell height mm above a flat cell (default = --step). Raise for more "
                         "LiDAR near/far separation without enlarging the tag (e.g. --bump_height 20).")
    ap.add_argument("--qr_px", type=int, default=256, help="QR raster resolution (default 256; "
                    "96 renders the QR + text chunky)")
    ap.add_argument("--qr_no_flip", action="store_true",
                    help="Do NOT pre-mirror the QR/text (debug). By default the QR is mirrored so it "
                         "renders right-side-up on the printed top; this is independent of --no_mirror_x "
                         "(which only affects the LiDAR bumps).")
    ap.add_argument("--qr_thresh", type=int, default=128, help="Threshold 0..255 (default 128)")
    ap.add_argument("--invert", action="store_true", help="Invert QR mask")
    ap.add_argument("--smooth", action="store_true",
                    help="Use LANCZOS resize for the QR/text overlay (default NEAREST). "
                         "Right for stylized text or logo overlays where smooth strokes matter "
                         "more than QR-module sharpness. Pair with a higher --qr_px (256+) so "
                         "the resampled strokes have enough pixels to stay clean.")
    ap.add_argument("--cap", type=float, default=0.24, help="Black top-cap thickness mm (default 0.24)")
    ap.add_argument("--allow-ambiguous", action="store_true",
                    help="Allow known ambiguous IDs reserved for testing only")
    ap.add_argument("--anchor", action="store_true",
                    help="(Deprecated no-op -- the anchor layout is the DEFAULT now. Use --legacy "
                         "for the old orientation-marker 4x4.)")
    ap.add_argument("--legacy", action="store_true",
                    help="Generate the LEGACY orientation-marker 4x4 (8-ID + CRC4) instead of the "
                         "DEFAULT anchor/fiducial layout. The anchor layout (A3-D13: 3 corner anchors "
                         "+ SECDED(12,7) over 12 data cells + (3,3) rotation guard, pose-corrected "
                         "decode) is the default -- far more robust on the VL53L5CX 8x8; anchor id "
                         "range 0..127. (Anchor tags print with no bump pre-mirror + no --bump_margin.)")
    ap.add_argument("--seam_gap", type=float, default=0.01,
                    help="Tiny z-gap mm between white and black at split to avoid z-fighting (default 0.01)")
    ap.add_argument("--bump_margin", type=float, default=0.0,
                    help="Shrink each bump in X/Y by this many mm on each side. Creates a low-height "
                         "gutter between adjacent cells so the LiDAR's bilinear sampler reads cleanly "
                         "at cell boundaries. Recommended ~10%% of cell size for dense patterns "
                         "(e.g. 2.5 for 100mm tag with 25mm cells). Default 0 (legacy behavior).")
    ap.add_argument("--no_mirror_x", action="store_true",
                    help="Force NO bump pre-mirror. This is the DEFAULT in anchor mode (bumps are raw "
                         "canonical, scanned bump-side toward the sensor), so you rarely need it. In "
                         "legacy mode it disables the print-and-flip pre-mirror.")
    ap.add_argument("--mirror_x", action="store_true",
                    help="Force bump pre-mirror (the default in LEGACY print-and-flip mode). In anchor "
                         "mode use only for a flip-to-face-below presentation. (The QR/text always "
                         "pre-mirrors regardless -- see --qr_no_flip.)")
    ap.add_argument("--border", type=float, default=5.0,
                    help="Width mm of a recessed moat ring around the bumps. Adds the moat OUTSIDE "
                         "the central tag size (so total print = size + 2*border). The recessed "
                         "perimeter gives the LiDAR a deterministic FAR cluster regardless of how "
                         "close the user holds the badge -- fixes the 'fills-FoV' failure mode where "
                         "k-means has only one cluster. Default 0 (no border).")
    ap.add_argument("--border_recess", type=float, default=4.0,
                    help="Depth mm to recess the moat below the print's flat baseline. Needs to be "
                         "deep enough to fall outside the engine's outlier filter (±25mm of median) "
                         "and into the FAR cluster of the k-means depth split. Default 4mm.")
    ap.add_argument("--square_border", action="store_true",
                    help="Keep the border a uniform square ring. By DEFAULT the border is TAPERED for "
                         "keyed insertion: full at the bottom (lower-left-anchor edge), left/right "
                         "borders taper to zero at the top corners, and the top border is removed -- so "
                         "the tag only drops into a matching stand one way up. Use this to opt out.")
    args = ap.parse_args()

    # Anchor is the DEFAULT layout now; --legacy opts into the old 4x4. (--anchor
    # is kept as a harmless no-op for old scripts.)
    anchor_mode = not args.legacy

    # Bump pre-mirror: anchor tags are raw-canonical (no mirror), legacy tags are
    # pre-mirrored for print-and-flip. --no_mirror_x / --mirror_x force either way.
    # (The QR/text mirror is independent -- qr_mirror_x below.)
    bump_mirror = (not anchor_mode)
    if args.no_mirror_x: bump_mirror = False
    if args.mirror_x:    bump_mirror = True

    # Sanity-check moat depth so the print isn't mechanically fragile.
    if args.border > 0.0 and args.border_recess >= args.base:
        print(f"WARNING: border_recess ({args.border_recess}) >= base ({args.base}); "
              f"the moat will be a hole through the print. Increase --base or "
              f"reduce --border_recess to keep the moat solid.")

    if anchor_mode:
        bits = build_anchor_grid(args.id)   # HR spec v2 (no rotation/CRC ambiguity gate needed)
    else:
        validate_id(args.id, allow_ambiguous=args.allow_ambiguous)
        bits = build_bit_grid(args.id, allow_ambiguous=args.allow_ambiguous)
    mask = load_qr_mask_stretched(args.qr, out_px=args.qr_px, thresh=args.qr_thresh,
                                  invert=args.invert, smooth=args.smooth)

    white_mesh, black_mesh = build_topcap_meshes(
        bits, mask, args.size, args.base, args.step, args.cap, args.seam_gap,
        bump_margin_mm=args.bump_margin,
        mirror_x=bump_mirror,
        qr_mirror_x=(not args.qr_no_flip),
        border_mm=args.border,
        border_recess_mm=args.border_recess,
        taper_border=(not args.square_border),
        bump_height_mm=args.bump_height,
    )

    model_xml = build_model_xml(white_mesh, black_mesh)
    write_3mf(args.out, model_xml, has_black=len(black_mesh.f) > 0)

    # Human-friendly cheat sheet (matches the scan/sweep tooling):
    #   A = anchor (fixed near/bump)   g = guard (fixed far/flat)
    #   # = raise this cell (near/1)   . = leave down (far/0)
    print(f"\nOmniTag cheat sheet -- ID {args.id}:")
    print("   A=anchor(fixed)  g=guard(fixed)  #=raise/near  .=down/far")
    for r in range(GRID_N):
        cells = []
        for c in range(GRID_N):
            if (r, c) in ANCHOR_CELLS:
                cells.append("A")
            elif (r, c) == GUARD_CELL:
                cells.append("g")
            else:
                cells.append("#" if bits[r][c] else ".")
        print("      " + " ".join(cells))
    print("   ORIENTATION: set the sliders EXACTLY as drawn, then present the tag")
    print("   bump-side AWAY from you (toward the sensor), ~80mm, no rotation --")
    print("   the guard 'g' is bottom-right, and pushing the bumps away applies the")
    print("   mirror for you, so there's nothing to flip.")
    print(f"white: v={len(white_mesh.v)} t={len(white_mesh.f)}")
    print(f"black: v={len(black_mesh.v)} t={len(black_mesh.f)}")
    print(f"wrote: {args.out}")
    print("Bambu Studio: assign Object 1=white, Object 2=black. Black prints only in the top cap layers.")


if __name__ == "__main__":
    main()
