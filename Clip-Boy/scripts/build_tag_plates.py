#!/usr/bin/env python3
"""
build_tag_plates.py -- assemble per-tag .3mf files into N SINGLE-PLATE Bambu
projects (9 tags per 256mm plate), black-cap IRONING set on every tag.

Why per-plate files (not one giant multi-plate project): Bambu keeps per-object
process overrides (ironing) only in a saved PROJECT and does NOT inherit them on
IMPORT. A single multi-plate file also blew the 30MB share limit AND mis-bound the
black filament (a combined mesh file gives each mesh a unique objectid, but Bambu
only maps model_settings part id=1/2 when each tag's white/black are objectid 1/2
in their OWN object file). So: 11 files, each a SINGLE-PLATE project that mirrors
the proven single-tag structure x9 -- open each, every black cap is already
flagged concentric/solid ironing + bound to extruder 2.

Input : tags/<id>.3mf (our 2-object white=1 / black=2 format, from build_all_tags)
Template: a working Bambu PROJECT 3mf with black-cap ironing (e.g. 42_ironed.3mf).
Output: <out-dir>/plate_01.3mf .. plate_NN.3mf

  py -3 scripts/build_tag_plates.py --template path/to/42_ironed.3mf \
        --out-dir tag_plates/ --per-plate 9
"""
import argparse
import csv
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAG_W, TAG_H = 70.0, 73.0
VERT_RE = re.compile(r'<vertex x="([-0-9.eE]+)" y="([-0-9.eE]+)" z="([-0-9.eE]+)"/>')
TRI_RE  = re.compile(r'<triangle v1="(\d+)" v2="(\d+)" v3="(\d+)"/>')
PROD = ('xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
        'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" '
        'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" '
        'requiredextensions="p"')


def catalog_ids():
    ids = set()
    with open(ROOT / "data" / "collectibles.csv", newline="", encoding="utf-8") as f:
        r = csv.reader(f); h = next(r); ic = h.index("ID")
        for row in r:
            try: ids.add(int(row[ic]))
            except (ValueError, IndexError): pass
    return sorted(ids)


def read_object(model_xml, oid):
    m = re.search(r'<object id="%s"[^>]*>.*?</object>' % oid, model_xml, re.S)
    if not m:
        return [], []
    blk = m.group(0)
    verts = [(float(a), float(b), float(c)) for a, b, c in VERT_RE.findall(blk)]
    tris = [(int(a), int(b), int(c)) for a, b, c in TRI_RE.findall(blk)]
    return verts, tris


def read_tag_3mf(path):
    with zipfile.ZipFile(path) as z:
        model = z.read("3D/3dmodel.model").decode()
    return read_object(model, "1"), read_object(model, "2")


def bbox_center(verts):
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    return ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0,
            (min(zs) + max(zs)) / 2.0)


def one_object_file(verts_faces_white, verts_faces_black, ox, oy, oz,
                    white_id, black_id, fidx):
    """A per-tag object_<fidx>.model holding two objects with GLOBALLY-UNIQUE
    objectids (white_id, black_id), each origin-centered by the SAME offset
    (ox,oy,oz) so they stay aligned.

    ⚠⚠ Bambu resolves a component's `objectid` GLOBALLY across the whole package,
    NOT within the referenced file. Reusing objectid 1/2 in every object file made
    every component resolve to the FIRST file's meshes -> all tags identical on the
    plate (a distinct p:UUID did NOT fix it; the objectid itself must be unique).
    The working reference (4_objects.3mf) uses tag i -> white=3i-2, black=3i-1,
    root=3i. UUIDs are file-index based (matches that reference)."""
    def obj(oid, uuid, vf):
        verts, tris = vf
        vs = "\n".join('<vertex x="%.6f" y="%.6f" z="%.6f"/>' % (x - ox, y - oy, z - oz)
                       for (x, y, z) in verts)
        ts = "\n".join('<triangle v1="%d" v2="%d" v3="%d"/>' % t for t in tris)
        return ('  <object id="%d" p:UUID="%s" type="model">\n   <mesh>\n'
                '    <vertices>\n%s\n    </vertices>\n'
                '    <triangles>\n%s\n    </triangles>\n   </mesh>\n  </object>\n'
                % (oid, uuid, vs, ts))
    body = obj(white_id, "%04d0000-81cb-4c03-9d28-80fed5dfa1dc" % fidx, verts_faces_white)
    has_black = bool(verts_faces_black[0])
    if has_black:
        body += obj(black_id, "%04d0001-81cb-4c03-9d28-80fed5dfa1dc" % fidx, verts_faces_black)
    xml = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" '
           'xml:lang="en-US" %s>\n <metadata name="BambuStudio:3mfVersion">1</metadata>\n'
           ' <resources>\n%s </resources>\n</model>\n' % (PROD, body))
    return xml, has_black


def write_plate_project(out_path, plate_tags, template_bytes, first_x, first_y,
                        stride_x, stride_y, cols, row0_dx):
    """plate_tags: list of (tag_id, (white_vf), (black_vf)). Writes ONE single-
    plate project 3mf mirroring the proven single-tag structure, x len(plate_tags)."""
    obj_files = {}          # path -> xml
    model_objs, build_items, ms_objects, rels = [], [], [], []
    for slot, (tid, wvf, bvf) in enumerate(plate_tags):
        fidx = slot + 1                     # 1-based file/tag index
        white_id = 3 * fidx - 2             # global objectids per 4_objects.3mf
        black_id = 3 * fidx - 1
        root_id = 3 * fidx                  # root model object + build item
        allv = wvf[0] + bvf[0]
        ox, oy, oz = bbox_center(allv)
        ofile_xml, has_black = one_object_file(wvf, bvf, ox, oy, oz,
                                               white_id, black_id, fidx)
        opath = "3D/Objects/object_%d.model" % fidx
        obj_files[opath] = ofile_xml

        comps = ('    <component p:path="/%s" objectid="%d" '
                 'transform="1 0 0 0 1 0 0 0 1 0 0 0"/>\n' % (opath, white_id))
        if has_black:
            comps += ('    <component p:path="/%s" objectid="%d" '
                      'transform="1 0 0 0 1 0 0 0 1 0 0 0"/>\n' % (opath, black_id))
        model_objs.append('  <object id="%d" p:UUID="%08x-61cb-4c03-9d28-80fed5dfa1dc" '
                          'type="model">\n   <components>\n%s   </components>\n  </object>\n'
                          % (root_id, root_id, comps))

        row = slot // cols
        gx = first_x + (slot % cols) * stride_x
        if row == 0:            # top row clears the top-left prime tower
            gx += row0_dx
        gy = first_y - row * stride_y
        build_items.append('  <item objectid="%d" p:UUID="%08x-b1ec-4553-aec9-835e5b724bb4" '
                           'transform="-1 0 0 0 -1 0 0 0 1 %.4f %.4f %.4f" printable="1"/>\n'
                           % (root_id, root_id, gx, gy, oz))

        # part id == the global component objectid (matches 4_objects.3mf binding)
        parts = ['    <part id="%d" subtype="normal_part">' % white_id,
                 '      <metadata key="name" value="HM_white"/>',
                 '      <metadata key="extruder" value="1"/>', '    </part>']
        if has_black:
            parts += ['    <part id="%d" subtype="normal_part">' % black_id,
                      '      <metadata key="name" value="HM_black_cap"/>',
                      '      <metadata key="extruder" value="2"/>',
                      '      <metadata key="ironing_pattern" value="concentric"/>',
                      '      <metadata key="ironing_type" value="solid"/>', '    </part>']
        ms_objects.append('  <object id="%d">\n    <metadata key="name" value="%d"/>\n'
                          '    <metadata key="extruder" value="1"/>\n%s\n  </object>'
                          % (root_id, tid, "\n".join(parts)))
        rels.append(' <Relationship Target="/%s" Id="rel-obj%d" '
                    'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
                    % (opath, fidx))

    model3d = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" '
               'xml:lang="en-US" %s>\n'
               ' <metadata name="Application">HRCode4x4-build_tag_plates</metadata>\n'
               ' <metadata name="BambuStudio:3mfVersion">1</metadata>\n <resources>\n%s'
               ' </resources>\n <build p:UUID="2c7c17d8-22b5-4d84-8835-1976022ea369">\n%s'
               ' </build>\n</model>\n' % (PROD, "".join(model_objs), "".join(build_items)))
    # NO <plate> block -- single plate (matches the proven single-tag project).
    model_settings = ('<?xml version="1.0" encoding="UTF-8"?>\n<config>\n%s\n</config>\n'
                      % "\n".join(ms_objects))

    with zipfile.ZipFile(template_bytes) as tpl:
        content_types = tpl.read("[Content_Types].xml").decode()
        project_settings = tpl.read("Metadata/project_settings.config")

    root_rel = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships '
                'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
                ' <Relationship Target="/3D/3dmodel.model" Id="rel-1" '
                'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>\n'
                '</Relationships>\n')
    model_rels = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships '
                  'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
                  '%s\n</Relationships>\n' % "\n".join(rels))

    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", root_rel)
        z.writestr("3D/3dmodel.model", model3d)
        z.writestr("3D/_rels/3dmodel.model.rels", model_rels)
        for path, xml in obj_files.items():
            z.writestr(path, xml)
        z.writestr("Metadata/model_settings.config", model_settings)
        z.writestr("Metadata/project_settings.config", project_settings)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tags-dir", type=Path, default=ROOT / "tags")
    ap.add_argument("--template", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, default=ROOT / "tag_plates")
    ap.add_argument("--ids", type=str, default=None)
    ap.add_argument("--per-plate", type=int, default=9)
    ap.add_argument("--cols", type=int, default=3)
    # 3x3 grid on one 256mm bed (owner-tuned in Studio). The prime tower sits
    # top-LEFT, so rows 2-3 sit LEFT (first-x) to use the full width under the
    # tower, while the TOP row alone is pushed +row0-dx right to clear it. This is
    # how 9 tags + the 35mm tower all fit 256mm: top row 74.25/146.25/218.25
    # (right edge 253), lower rows 41.25/113.25/185.25 (left edge 6).
    ap.add_argument("--first-x", type=float, default=41.25)
    ap.add_argument("--first-y", type=float, default=206.0)
    ap.add_argument("--stride-x", type=float, default=72.0)
    ap.add_argument("--stride-y", type=float, default=78.0)
    ap.add_argument("--row0-dx", type=float, default=33.0,
                    help="Extra +X shift applied to the TOP row only, to clear the "
                         "top-left prime tower (owner-tuned: 33mm -> top row at "
                         "74.25/146.25/218.25).")
    args = ap.parse_args()

    ids = ([int(x) for x in args.ids.split(",")] if args.ids else catalog_ids())
    ids = [i for i in ids if (args.tags_dir / f"{i}.3mf").exists()]
    if not ids:
        raise SystemExit(f"no tag .3mf files in {args.tags_dir}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    plates = [ids[i:i + args.per_plate] for i in range(0, len(ids), args.per_plate)]
    for pno, plate_ids in enumerate(plates, 1):
        plate_tags = []
        for tid in plate_ids:
            wvf, bvf = read_tag_3mf(args.tags_dir / f"{tid}.3mf")
            if not wvf[0]:
                print(f"  skip {tid}: no white mesh"); continue
            plate_tags.append((tid, wvf, bvf))
        out = args.out_dir / f"plate_{pno:02d}.3mf"
        write_plate_project(out, plate_tags, args.template, args.first_x,
                            args.first_y, args.stride_x, args.stride_y, args.cols,
                            args.row0_dx)
        mb = out.stat().st_size / 1e6
        print(f"  plate {pno:02d}: {len(plate_tags)} tags -> {out.name} ({mb:.0f} MB)")
    print(f"wrote {len(plates)} single-plate project(s) to {args.out_dir}")


if __name__ == "__main__":
    main()
