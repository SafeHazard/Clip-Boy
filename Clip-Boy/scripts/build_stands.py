#!/usr/bin/env python3
"""
build_stands.py -- emit two .3mf tag-stand designs for HR-code prints.

Both stands are simple Y-Z extrusions along the X (tag-length) axis:
  - stand_flat.3mf:  low-profile flat block with a 22.5 mm slot on top
                     (slot offset 2.25 mm toward the front for a natural
                     ~3 deg backward lean against the tag's bump weight)
  - stand_wedge.3mf: taller wedge with a vertical back wall and a chamfered
                     front face; tag stands near-vertical in the slot

No external dependencies: builds the mesh as explicit vertex + triangle
arrays, then zip-packs a minimal .3mf the same way hm_codegen.py does.

Usage:
  py -3 scripts/build_stands.py
"""

import os
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "stands"

TAG_LENGTH = 70.0  # mm; matches the 60 + 5*2 (border) tag width


# --------------------------------------------------------------------------
# Generic extrusion of a CCW 2D profile along the X axis.
# --------------------------------------------------------------------------

def _write_3mf(verts, tris, out_path):
    """Pack a single-object 3MF zip with a minimal model XML."""
    v_xml = "\n".join(
        f'        <vertex x="{x:.6f}" y="{y:.6f}" z="{z:.6f}"/>'
        for (x, y, z) in verts
    )
    t_xml = "\n".join(
        f'        <triangle v1="{a}" v2="{b}" v3="{c}"/>'
        for (a, b, c) in tris
    )
    model_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<model unit="millimeter" xml:lang="en-US"\n'
        '  xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">\n'
        '  <resources>\n'
        '    <object id="1" type="model">\n'
        '      <mesh>\n'
        '        <vertices>\n'
        f'{v_xml}\n'
        '        </vertices>\n'
        '        <triangles>\n'
        f'{t_xml}\n'
        '        </triangles>\n'
        '      </mesh>\n'
        '    </object>\n'
        '  </resources>\n'
        '  <build>\n'
        '    <item objectid="1"/>\n'
        '  </build>\n'
        '</model>\n'
    )
    content_types = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n'
        '  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>\n'
        '  <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>\n'
        '</Types>\n'
    )
    rels = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
        '  <Relationship Id="rel0"\n'
        '    Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"\n'
        '    Target="/3D/3dmodel.model"/>\n'
        '</Relationships>\n'
    )
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("3D/3dmodel.model", model_xml)


def _extrude(profile_yz, boundary_edges, endcap_tris, length):
    """
    Build a watertight mesh by extruding a 2D profile along +X.

    profile_yz:     [(y, z), ...] -- ordered CCW (math convention)
    boundary_edges: [(i_from, i_to), ...] -- ordered CCW around the boundary,
                    using indices into profile_yz
    endcap_tris:    [(i, j, k), ...] -- triangulation of the polygon interior,
                    each triangle in CCW order (looking from -X)
    length:         extrusion length along +X

    Returns (verts, tris). verts[i] is profile vertex i at x=0;
    verts[n + i] is the same vertex at x=length.
    """
    n = len(profile_yz)
    verts = []
    for (y, z) in profile_yz:
        verts.append((0.0, y, z))
    for (y, z) in profile_yz:
        verts.append((length, y, z))

    tris = []
    # x=0 endcap: triangles already CCW from -X view -> normals point -X. ✓
    for (i, j, k) in endcap_tris:
        tris.append((i, j, k))
    # x=L endcap: reverse winding -> normals point +X.
    for (i, j, k) in endcap_tris:
        tris.append((n + i, n + k, n + j))
    # Side walls: for each CCW boundary edge a->b, emit two triangles whose
    # outward normal is to the RIGHT of (a->b) in the YZ plane (= away from
    # the polygon interior).
    for (a, b) in boundary_edges:
        # Quad corners: A0 = a@x=0, AL = a@x=L, B0 = b@x=0, BL = b@x=L.
        # Outward winding pair (verified by cross product):
        tris.append((a, n + b, n + a))   # A0, BL, AL
        tris.append((a, b,     n + b))   # A0, B0, BL
    return verts, tris


# --------------------------------------------------------------------------
# Stand A -- flat base, 70 x 27 x 10 mm with a 22.5 mm slot.
# --------------------------------------------------------------------------

def build_flat(out_path):
    # Profile augmented with two aux midpoints (0, 2) and (27, 2) so the
    # interior can be cleanly split into 3 axis-aligned rectangles.
    #
    #   z=10  p8─p7    p4─p3
    #          │ │  SLOT │ │
    #   z=2   p9─p6    p5─p2
    #          │              │
    #   z=0   p0──────────────p1
    #          y=0           y=27
    profile = [
        (0.0,   0.0),    # p0
        (27.0,  0.0),    # p1
        (27.0,  2.0),    # p2 aux
        (27.0, 10.0),    # p3
        (24.75,10.0),    # p4   slot right at top
        (24.75, 2.0),    # p5   slot right at floor
        (2.25,  2.0),    # p6   slot left at floor
        (2.25, 10.0),    # p7   slot left at top
        (0.0,  10.0),    # p8
        (0.0,   2.0),    # p9 aux
    ]
    boundary_edges = [
        (0, 1), (1, 2), (2, 3), (3, 4), (4, 5),
        (5, 6), (6, 7), (7, 8), (8, 9), (9, 0),
    ]
    endcap_tris = [
        (0, 1, 2), (0, 2, 9),    # bottom strip  (z=0..2)
        (5, 2, 3), (5, 3, 4),    # right column  (y=24.75..27)
        (9, 6, 7), (9, 7, 8),    # left column   (y=0..2.25)
    ]
    verts, tris = _extrude(profile, boundary_edges, endcap_tris, TAG_LENGTH)
    _write_3mf(verts, tris, out_path)


# --------------------------------------------------------------------------
# Stand B -- wedge, 70 x 35 x 25 mm with a 22 mm slot and a chamfered front.
# --------------------------------------------------------------------------

def build_wedge(out_path):
    # Profile: rectangle with the front bottom-left corner sliced off as a
    # diagonal (slope from y=0,z=0 to y=10,z=25), plus a top slot.
    #
    #   z=25  p8     p5─p4
    #          \      │ │
    #          (slope) │ │   SLOT is the gap p5-p6-p7-p8 region
    #          p1─p7──p6 │  (slot floor at z=17)
    #   z=17       \    │ │
    #               \   │ │
    #                \  │ │
    #   z=0   p0──────p1?────p2(35,0)
    #          y=0  y=10    y=35
    #
    # NOTE p1 is the bottom of the slope at y=10, z=0 (aux). p3 is y=35,z=17
    # (aux) so the bottom-right rect and top-right rect share a cleaner edge.
    profile = [
        (0.0,   0.0),    # p0
        (10.0,  0.0),    # p1 aux  (bottom of slope on the front face)
        (35.0,  0.0),    # p2
        (35.0, 17.0),    # p3 aux  (back wall at slot-floor height)
        (35.0, 25.0),    # p4
        (32.0, 25.0),    # p5
        (32.0, 17.0),    # p6  slot floor right
        (10.0, 17.0),    # p7  slot floor left
        (10.0, 25.0),    # p8  slot top-left / top of slope
    ]
    boundary_edges = [
        (0, 1), (1, 2), (2, 3), (3, 4), (4, 5),
        (5, 6), (6, 7), (7, 8), (8, 0),
    ]
    endcap_tris = [
        (0, 1, 8),               # slope-wedge triangle (y=0..10, beneath slope)
        (1, 2, 3), (1, 3, 7),    # bottom-right rect    (y=10..35, z=0..17)
        (6, 3, 4), (6, 4, 5),    # top-right rect       (y=32..35, z=17..25)
    ]
    verts, tris = _extrude(profile, boundary_edges, endcap_tris, TAG_LENGTH)
    _write_3mf(verts, tris, out_path)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    flat = OUT_DIR / "stand_flat.3mf"
    wedge = OUT_DIR / "stand_wedge.3mf"
    build_flat(str(flat))
    build_wedge(str(wedge))
    print(f"Wrote {flat.relative_to(ROOT)}")
    print(f"Wrote {wedge.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
