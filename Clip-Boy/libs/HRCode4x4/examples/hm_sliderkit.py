#!/usr/bin/env python3
"""
hm_sliderkit.py

Reconfigurable A3-D13 "OmniTag" HR tag as a single print-in-place 3MF.

The tag is a 4x4 grid (HR spec DC34-155, see HR-OMNITAG-HANDOFF.md):

  - 3 fixed NEAR anchors at corners (0,0)(0,3)(3,0) + 1 fixed FAR guard at (3,3)
    -- the anchor L is the scanner's fiducial (pose + rotation). Built as RIGID
    fused posts: anchors permanently NEAR, guard permanently FAR-flat. They never
    move.
  - The 12 non-corner cells are SETTABLE sliders carrying a SECDED(12,7) codeword
    for the 7-bit ID. Dial any of the 95 collectible IDs from omnitag_id_table.txt
    (or `--id N` prints the setting cheat-sheet).

Each settable cell is a 14x14 CAP on a narrower STEM, dovetail-captured in a
hidden frame channel so the caps sit edge-to-edge (no gaps break the reading
surface). NEAR = cap raised code_rise mm (bit 1); FAR = cap down flush (bit 0).
Two hard stops bound the travel -- cap underside on the divider (FAR), rib head
on the groove roof (NEAR) -- and the dovetail head is wider than the throat, so
a slider can't fall out either end. A recessed moat ring gives the LiDAR its
far-distance reference (handoff s5.5). Held by friction (no detents yet); the
hard stops make each cell crisp fully-NEAR or fully-FAR per handoff s5.1.

The frame height auto-scales to --code_rise so the dovetail stays captured across
the full travel. Geometry is axis-aligned box CSG meshed by a small rectilinear
mesher (rect_mesh) -> every object is watertight and manifold; the 45-deg dovetail
flares are a layer-height staircase (what a slicer makes anyway). Anchor/guard/
SECDED layout come from hm_codegen (the one canonical Python copy, CLAUDE.md).

The .3mf is emitted ALREADY LAID ON ITS SIDE (dovetails left-right, slide axis
horizontal): the 45-deg flares print as self-supporting staircases and the layer
lines run along the slide -- drop it in the slicer and print, no re-orienting.
(--upright keeps the vertical model frame for debugging.) Print bump-side toward
the sensor, un-mirrored (handoff s4).

Examples:
  # The OmniTag (default): 14mm near/far, recessed moat, 0.40mm slide clearance.
  python hm_sliderkit.py --clearance 0.40 --out omnitag.3mf

  # ... plus a setting cheat-sheet for an ID (e.g. Gandalf = 16).
  python hm_sliderkit.py --clearance 0.40 --id 16 --out omnitag.3mf

  # Slider fit-test coupon first (prints in minutes); match --clearance.
  python hm_sliderkit.py --clearance 0.40 --calibration --out calib-40.3mf

Dial in --clearance with a --calibration coupon before committing the full tag.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hm_codegen import (  # noqa: E402
    GRID_N,
    ANCHOR_CELLS,
    GUARD_CELL,
    secded_encode,
    build_anchor_grid,
    Mesh,
    add_quad,
)
from hm_blockkit import write_3mf  # noqa: E402

# Cell roles for the A3-D13 OmniTag, in this file's (i=col, j=row) index space.
# 3 fixed NEAR anchors + 1 fixed FAR guard occupy the 4 corners; the 12
# non-corner cells are the settable SECDED data cells (sliders).
OMNI_ANCHORS = {(c, r) for (r, c) in ANCHOR_CELLS}     # {(0,0),(3,0),(0,3)}
OMNI_GUARD = (GUARD_CELL[1], GUARD_CELL[0])            # (3,3)


def omni_roles(nx, ny):
    """(i,j) -> 'anchor' | 'guard' | 'data' for an nx x ny grid."""
    roles = {}
    for i in range(nx):
        for j in range(ny):
            if (i, j) in OMNI_ANCHORS:
                roles[(i, j)] = 'anchor'
            elif (i, j) == OMNI_GUARD:
                roles[(i, j)] = 'guard'
            else:
                roles[(i, j)] = 'data'
    return roles


# ----------------------------
# Rectilinear box-CSG mesher
# ----------------------------
def _faces(mesh, x0, y0, z0, x1, y1, z1, nxm, nxp, nym, nyp, nzm, nzp):
    """Emit boundary faces of cell [x0,x1]x[y0,y1]x[z0,z1] whose neighbor flags
    are empty. Windings match add_box in hm_blockkit (outward normals)."""
    if nzm:
        add_quad(mesh, (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0))
    if nzp:
        add_quad(mesh, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))
    if nym:
        add_quad(mesh, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))
    if nyp:
        add_quad(mesh, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1))
    if nxm:
        add_quad(mesh, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1))
    if nxp:
        add_quad(mesh, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))


def _axis_breaks(boxes, lo_idx, hi_idx, clamp_lo, clamp_hi, eps=1e-6):
    vals = {clamp_lo, clamp_hi}
    for b in boxes:
        for v in (b[lo_idx], b[hi_idx]):
            if clamp_lo - eps < v < clamp_hi + eps:
                vals.add(min(max(v, clamp_lo), clamp_hi))
    return sorted(vals)


def _inside(boxes, x, y, z):
    for (ax0, ay0, az0, ax1, ay1, az1) in boxes:
        if ax0 < x < ax1 and ay0 < y < ay1 and az0 < z < az1:
            return True
    return False


def rect_mesh(add_boxes, sub_boxes):
    """Mesh (union of add_boxes) minus (union of sub_boxes). Breakpoints come
    from every box face, so axis-aligned features land exactly and flat spans
    collapse to one quad. Watertight + manifold by construction."""
    mesh = Mesh(v=[], f=[])
    if not add_boxes:
        return mesh
    bx0 = min(b[0] for b in add_boxes); bx1 = max(b[3] for b in add_boxes)
    by0 = min(b[1] for b in add_boxes); by1 = max(b[4] for b in add_boxes)
    bz0 = min(b[2] for b in add_boxes); bz1 = max(b[5] for b in add_boxes)
    allb = list(add_boxes) + list(sub_boxes)
    xs = _axis_breaks(allb, 0, 3, bx0, bx1)
    ys = _axis_breaks(allb, 1, 4, by0, by1)
    zs = _axis_breaks(allb, 2, 5, bz0, bz1)

    nx, ny, nz = len(xs) - 1, len(ys) - 1, len(zs) - 1
    occ = [[[False] * nz for _ in range(ny)] for _ in range(nx)]
    cx = [(xs[i] + xs[i + 1]) * 0.5 for i in range(nx)]
    cy = [(ys[j] + ys[j + 1]) * 0.5 for j in range(ny)]
    cz = [(zs[k] + zs[k + 1]) * 0.5 for k in range(nz)]
    for i in range(nx):
        for j in range(ny):
            for k in range(nz):
                if _inside(add_boxes, cx[i], cy[j], cz[k]) and \
                        not _inside(sub_boxes, cx[i], cy[j], cz[k]):
                    occ[i][j][k] = True

    def empty(i, j, k):
        if i < 0 or i >= nx or j < 0 or j >= ny or k < 0 or k >= nz:
            return True
        return not occ[i][j][k]

    for i in range(nx):
        for j in range(ny):
            for k in range(nz):
                if not occ[i][j][k]:
                    continue
                _faces(mesh, xs[i], ys[j], zs[k], xs[i + 1], ys[j + 1], zs[k + 1],
                       empty(i - 1, j, k), empty(i + 1, j, k),
                       empty(i, j - 1, k), empty(i, j + 1, k),
                       empty(i, j, k - 1), empty(i, j, k + 1))
    return mesh


# ----------------------------
# Geometry
# ----------------------------
class Geom:
    """All Z is measured from the bed (z=0). At the LOW datum a block's cap top
    sits at R (the rim); HIGH adds `travel`. The stem rides in a frame channel
    whose dividers stop at D_top == the cap underside at LOW."""

    def __init__(self, a, nx=GRID_N, ny=GRID_N, moat_w=None):
        self.nx = nx                              # pockets in X (full kit = GRID_N)
        self.ny = ny                              # pockets in Y
        self.pitch = a.pitch
        self.field_x = self.nx * self.pitch       # edge-to-edge cap envelope
        self.field_y = self.ny * self.pitch
        self.C = a.clearance
        self.OW = a.wall                           # groove-perimeter width
        self.moat_w = a.moat_w if moat_w is None else moat_w   # recessed FAR-ref ring
        self.moat_recess = a.moat_recess
        self.pad = self.OW + self.moat_w           # origin -> field offset
        self.cap = self.pitch - a.cap_gap          # cap footprint (square)
        self.cap_gap = a.cap_gap
        self.Ch = a.cap_h                          # cap height
        self.Sw = a.stem                           # stem footprint (square)
        self.travel = a.code_rise                  # low->high rise (the whole point)
        self.roof = a.roof                         # groove-roof thickness (high stop)
        self.head_h = a.head_h
        self.base = a.base                         # solid divider below the groove
        self.neck_w = a.neck_w                     # dovetail throat width (at opening)
        self.head_w = a.head_w                     # dovetail width at full depth
        self.dove_steps = a.dove_steps             # staircase steps approximating 45 deg
        self.dove_d = (self.head_w - self.neck_w) / 2.0   # 45 deg flare depth
        # Heights are derived BOTTOM-UP from the travel so the frame is always
        # tall enough to keep the dovetail captured across the full rise.
        # (block-local; LOW print offset is one clearance.)
        self.cz0 = self.base                       # chamber floor (not a stop)
        self.hz0 = self.cz0 + a.bottom_gap         # head bottom at LOW
        self.hz1 = self.hz0 + self.head_h          # head top at LOW
        self.cz1 = self.hz1 + self.travel          # groove roof underside (high stop)
        self.D_top = self.cz1 + self.roof          # divider top = cap underside (low)
        self.R = self.D_top + self.Ch              # FAR level / low cap-top height
        self.NEAR = self.R + self.travel           # NEAR level / high cap-top (anchor top)
        self.chan = self.Sw + 2 * self.C           # stem channel
        self.moat_top = max(0.0, self.R - self.moat_recess)   # recessed FAR reference
        self.outer_x = self.field_x + 2 * self.pad
        self.outer_y = self.field_y + 2 * self.pad
        self.high_h = self.NEAR

    def cc(self, i):
        return self.pad + self.pitch * (i + 0.5)

    def validate(self):
        if self.hz0 <= 0:
            raise SystemExit("base/bottom_gap too small; raise --base.")
        throat = self.neck_w + 2 * self.C
        if self.head_w <= throat:
            raise SystemExit(
                f"head_w ({self.head_w}) must exceed throat ({throat:.2f}=neck_w+2*clearance).")
        divider = self.pitch - self.chan
        if 2 * (self.dove_d + self.C) > divider + 1e-9:
            print(f"NOTE: divider ({divider:.1f}mm) is thin for two grooves of depth "
                  f"{self.dove_d + self.C:.1f}; raise --pitch or shrink the rib.")
        if self.cap >= self.pitch:
            raise SystemExit("cap_gap must be > 0 so caps don't fuse to neighbours.")


def _dovetail(x_root, dirn, z0, z1, w_root, dove_d, yc, steps):
    """Boxes approximating a 45-deg dovetail flare protruding from x_root in
    direction dirn (+1/-1): width w_root at the opening, widening to
    w_root+2*dove_d at depth dove_d, extruded over [z0,z1]. Each step uses its
    deeper (wider) edge so the staircase is fully backed -- exactly the layer
    staircase a slicer makes from a smooth 45-deg face."""
    n = max(1, steps)
    dx = dove_d / n
    boxes = []
    for s in range(n):
        d1 = (s + 1) * dx
        half = (w_root + 2 * d1) / 2.0
        a = x_root + dirn * s * dx
        b = x_root + dirn * d1
        boxes.append((min(a, b), yc - half, z0, max(a, b), yc + half, z1))
    return boxes


def _frame_boxes(g: Geom, roles: dict):
    """(add, sub) box lists for the frame. Data cells get a slider channel +
    dovetail grooves; anchor/guard cells stay solid and get a fixed post on top
    (NEAR for anchors, FAR-flat for the guard). A recessed moat ring around the
    field gives the LiDAR its far reference. Shared by the mesher and the ASCII
    debug so they can't drift."""
    mw = g.moat_w
    add = []
    if mw > 1e-9:
        add.append((0.0, 0.0, 0.0, g.outer_x, g.outer_y, g.moat_top))   # recessed ring
    # inner (perimeter + field) solid up to the divider tops
    add.append((mw, mw, 0.0, g.outer_x - mw, g.outer_y - mw, g.D_top))
    sub = []
    for i in range(g.nx):
        cx = g.cc(i)
        sxl, sxr = cx - g.Sw / 2, cx + g.Sw / 2     # stem faces
        for j in range(g.ny):
            if roles.get((i, j), 'data') != 'data':
                continue                            # anchors/guard stay solid
            cy = g.cc(j)
            y0, y1 = cy - g.chan / 2, cy + g.chan / 2
            # Stem channel (open bottom, up through the divider tops).
            sub.append((cx - g.chan / 2, y0, -1.0, cx + g.chan / 2, y1, g.D_top + 1e-3))
            # Dovetail grooves over the travel range: rib flare grown by C, plus
            # C of back-clearance behind the head.
            hw = g.head_w / 2 + g.C
            for sxf, dirn in ((sxl, -1), (sxr, +1)):
                sub.extend(_dovetail(sxf, dirn, g.cz0, g.cz1,
                                     g.neck_w + 2 * g.C, g.dove_d, cy, g.dove_steps))
                back = sxf + dirn * (g.dove_d + g.C)
                tip = sxf + dirn * g.dove_d
                sub.append((min(tip, back), cy - hw, g.cz0, max(tip, back), cy + hw, g.cz1))
    # Fixed corner posts: anchors permanently NEAR, guard permanently FAR-flat.
    for (i, j), role in roles.items():
        if role not in ('anchor', 'guard'):
            continue
        cx, cy = g.cc(i), g.cc(j)
        top = g.NEAR if role == 'anchor' else g.R
        add.append((cx - g.cap / 2, cy - g.cap / 2, g.D_top, cx + g.cap / 2, cy + g.cap / 2, top))
    return add, sub


def build_frame(g: Geom, roles: dict):
    add, sub = _frame_boxes(g, roles)
    return rect_mesh(add, sub)


def build_block(g: Geom, i: int, j: int, raised: bool):
    """Cap + stem + two dovetail ribs for cell (i,j). Printed lifted by one
    clearance off the low stop (raised=False) so no stop face prints fused."""
    cx, cy = g.cc(i), g.cc(j)
    dz = g.C + (g.travel if raised else 0.0)
    caph, stemh = g.cap / 2, g.Sw / 2
    add = [(cx - caph, cy - caph, g.D_top + dz, cx + caph, cy + caph, g.R + dz)]   # cap
    add.append((cx - stemh, cy - stemh, dz, cx + stemh, cy + stemh, g.D_top + dz))  # stem
    rz0, rz1 = g.hz0 + dz, g.hz1 + dz
    for sxf, dirn in ((cx - stemh, -1), (cx + stemh, +1)):
        add.extend(_dovetail(sxf, dirn, rz0, rz1, g.neck_w, g.dove_d, cy, g.dove_steps))
    return rect_mesh(add, [])


# ----------------------------
# ASCII cross-section debug
# ----------------------------
def ascii_xz(g: Geom, roles: dict, j=0):
    """X-Z slice through stem row j (Z up, X right). '#'=frame."""
    print(f"\nX-Z slice through stem row j={j} (Z up, X right):")
    cy = g.cc(j)
    fa, fs = _frame_boxes(g, roles)
    step = 0.5
    nz = int(g.high_h / step) + 2
    nx = int(g.outer_x / step) + 1
    for kz in range(nz, -1, -1):
        z = kz * step
        row = "".join('#' if (_inside(fa, ix * step, cy, z) and not _inside(fs, ix * step, cy, z))
                      else '.' for ix in range(nx))
        print(f"{z:5.1f} {row}")


def omni_cheat(id7: int) -> str:
    """Setting guide: which of the 12 data cells to raise (NEAR) for an ID."""
    grid = build_anchor_grid(id7)                          # 4x4, 1 = NEAR
    seq = secded_encode(id7)
    lines = [f"OmniTag cheat sheet -- ID {id7}  (SECDED codeword = {''.join(map(str, seq))})",
             "   A=anchor(fixed)  g=guard(fixed)  #=raise/near  .=down/far",
             ""]
    near = 0
    for r in range(GRID_N):
        cells = []
        for c in range(GRID_N):
            if (r, c) in ANCHOR_CELLS:
                cells.append('A')
            elif (r, c) == GUARD_CELL:
                cells.append('g')
            else:
                up = grid[r][c] == 1
                near += up
                cells.append('#' if up else '.')
        lines.append("      " + " ".join(cells))
    lines += [
        "",
        f"  raise {near} of the 12 data cells to NEAR; leave the rest FAR.",
        "  ORIENTATION: set the sliders EXACTLY as drawn, then present the tag",
        "  bump-side AWAY from you (toward the sensor), ~80mm, no rotation --",
        "  guard 'g' is bottom-right; pushing the bumps away applies the mirror,",
        "  so there's nothing to flip.",
    ]
    return "\n".join(lines)


# ----------------------------
# Main
# ----------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Reconfigurable A3-D13 OmniTag: one print-in-place 3MF with 12 slider "
                    "data cells + 3 fixed NEAR anchors + 1 fixed FAR guard.")
    ap.add_argument("--out", required=True, help="Output .3mf")
    ap.add_argument("--id", type=int, default=None, help="Also print an ID setting cheat-sheet (0..127)")
    ap.add_argument("--pitch", type=float, default=14.0, help="Cell pitch mm (4*pitch grid, default 14 per spec)")
    ap.add_argument("--cap_gap", type=float, default=0.3, help="Gap between adjacent caps mm (default 0.3)")
    ap.add_argument("--cap_h", type=float, default=2.0, help="Cap (overhang) height mm (default 2)")
    ap.add_argument("--stem", type=float, default=9.0, help="Stem footprint mm (square, default 9)")
    ap.add_argument("--code_rise", type=float, default=14.0,
                    help="NEAR->FAR travel mm = the near/far separation (default 14, ~10mm at the sensor). "
                         "The frame height auto-scales to keep the dovetail captured across it.")
    ap.add_argument("--base", type=float, default=2.0,
                    help="Solid divider depth below the groove mm (default 2)")
    ap.add_argument("--clearance", type=float, default=0.35, help="Print-in-place gap per side mm (default 0.35)")
    ap.add_argument("--wall", type=float, default=5.0, help="Groove-perimeter width mm (default 5)")
    ap.add_argument("--moat_w", type=float, default=5.0, help="Recessed FAR-reference moat ring width mm (default 5)")
    ap.add_argument("--moat_recess", type=float, default=5.0, help="Moat depth below FAR level mm (default 5)")
    ap.add_argument("--roof", type=float, default=2.0, help="Groove-roof thickness mm = high stop (default 2)")
    ap.add_argument("--head_h", type=float, default=4.0, help="Rib head height mm (default 4)")
    ap.add_argument("--bottom_gap", type=float, default=1.0, help="Chamber floor gap below head mm (default 1)")
    ap.add_argument("--neck_w", type=float, default=2.5, help="Dovetail throat width mm (default 2.5)")
    ap.add_argument("--head_w", type=float, default=4.5, help="Dovetail full-depth width mm (default 4.5)")
    ap.add_argument("--dove_steps", type=int, default=4,
                    help="Staircase steps approximating the 45-deg flare (default 4; more = smoother/heavier)")
    ap.add_argument("--calibration", action="store_true",
                    help="Emit a small 2x1 slider fit-test coupon (no anchors/moat) instead of the OmniTag. "
                         "Prints in minutes; run one per candidate --clearance to dial in the slide fit.")
    ap.add_argument("--upright", action="store_true",
                    help="Keep the slide axis vertical (model frame). By DEFAULT the .3mf is laid "
                         "ON ITS SIDE -- dovetails running left-right, slide axis horizontal -- so it "
                         "drops straight into the slicer in the right print orientation.")
    ap.add_argument("--ascii", action="store_true", help="Print an X-Z cross-section for sanity checking")
    args = ap.parse_args()

    if args.calibration:
        g = Geom(args, nx=2, ny=1, moat_w=0.0)     # plain slider coupon, no anchors
        roles = {}                                  # all data
        data_cells = [(0, 0), (1, 0)]
    else:
        g = Geom(args, nx=GRID_N, ny=GRID_N)
        roles = omni_roles(GRID_N, GRID_N)
        data_cells = [ij for ij, r in roles.items() if r == 'data']
    g.validate()

    objects = [("HR_frame", build_frame(g, roles), 1)]
    for (i, j) in sorted(data_cells):
        objects.append((f"Data_{j}{i}", build_block(g, i, j, raised=False), 1))

    if not args.upright:
        # Lay it on its side for printing: rotate +90 deg about X so the slide
        # axis (model Z) becomes horizontal depth and the dovetails (model X)
        # run left-right across the bed. Layer lines then run along the slide
        # (low friction) and both dovetail flares present 45-deg faces.
        for (_n, m, _e) in objects:
            m.v = [(x, g.high_h - z, y) for (x, y, z) in m.v]
        # Recenter so the assembly sits at the origin with z=0 on the bed.
        allv = [p for (_n, m, _e) in objects for p in m.v]
        ox = min(p[0] for p in allv); oy = min(p[1] for p in allv); oz = min(p[2] for p in allv)
        for (_n, m, _e) in objects:
            m.v = [(x - ox, y - oy, z - oz) for (x, y, z) in m.v]

    write_3mf(args.out, objects)

    n_tri = sum(len(m.f) for (_n, m, _e) in objects)
    n_anchor = sum(1 for r in roles.values() if r == 'anchor')
    n_guard = sum(1 for r in roles.values() if r == 'guard')
    tag = "CALIBRATION coupon" if args.calibration else "A3-D13 OmniTag"
    print(f"{tag}: frame {g.outer_x:.1f} x {g.outer_y:.1f}mm")
    print(f"grid: {g.nx}x{g.ny}, pitch {g.pitch:.0f}mm ({g.field_x:.0f}x{g.field_y:.0f}mm of "
          f"{g.cap:.1f}mm caps); {len(data_cells)} sliders, {n_anchor} fixed NEAR anchors, {n_guard} FAR guard")
    print(f"levels: FAR {g.R:.0f} -> NEAR {g.NEAR:.0f}mm (travel {g.travel:.0f}); "
          f"moat recessed to {g.moat_top:.0f}mm; frame body {g.D_top:.0f}mm")
    print(f"objects={len(objects)}  triangles={n_tri}  clearance={g.C}mm per side")
    print(f"wrote: {args.out}")
    if args.ascii:
        ascii_xz(g, roles, j=1)
    if args.id is not None and not args.calibration:
        print()
        print(omni_cheat(args.id))


if __name__ == "__main__":
    main()
