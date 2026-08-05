#!/usr/bin/env py -3
# Is HR code 7 <-> 48 a REFLECTION-ambiguous pair the rotation-only scheme missed?
# Replicates hm_codegen encode + decode_hr (rotation-only) decode, then feeds all 8
# dihedral orientations of code 7's tag through the decoder.
GN = 4
ORIENT = {(0,0):1,(0,1):1,(1,0):1,(1,1):0}
ID_CELLS = [(0,2),(0,3),(1,2),(1,3),(2,0),(2,1),(2,2),(2,3)]
CRC_CELLS = [(3,0),(3,1),(3,2),(3,3)]
crc4 = lambda x: (x ^ (x>>4)) & 0xF

def encode(id8):
    g=[[0]*GN for _ in range(GN)]
    for (r,c),v in ORIENT.items(): g[r][c]=v
    for i,(r,c) in enumerate(ID_CELLS): g[r][c]=(id8>>i)&1
    cc=crc4(id8)
    for i,(r,c) in enumerate(CRC_CELLS): g[r][c]=(cc>>i)&1
    return g

def rot90(g): return [[g[GN-1-r][c] for r in range(GN)] for c in range(GN)]
def flipH(g): return [row[::-1] for row in g]            # mirror across vertical axis (reverse cols)
def flipV(g): return g[::-1]                             # mirror across horizontal axis (reverse rows)
def transpose(g): return [[g[c][r] for c in range(GN)] for r in range(GN)]

def orient_ok(g): return all(g[r][c]==v for (r,c),v in ORIENT.items())

def decode(g0):
    g=[row[:] for row in g0]
    for rot in range(4):
        if orient_ok(g):
            idv=sum((g[r][c]&1)<<i for i,(r,c) in enumerate(ID_CELLS))
            cr =sum((g[r][c]&1)<<i for i,(r,c) in enumerate(CRC_CELLS))
            return idv, (cr==crc4(idv)), rot
        g=rot90(g)
    return None, False, None

g7 = encode(7)
print("code 7 canonical grid:");  [print("  ", r) for r in g7]
print("code 7 flat row-major:", " ".join(str(b) for row in g7 for b in row))
print()

# all 8 dihedral orientations
def named_orientations(g):
    yield "identity", g
    yield "rot90",  rot90(g)
    yield "rot180", rot90(rot90(g))
    yield "rot270", rot90(rot90(rot90(g)))
    yield "flipH(mirror vert-axis)", flipH(g)
    yield "flipV(mirror horiz-axis)", flipV(g)
    yield "transpose(diag)", transpose(g)
    yield "anti-transpose", flipH(rot90(g))

print("how each PHYSICAL orientation of the code-7 tag decodes (rotation-only decoder):")
for name, gg in named_orientations(g7):
    idv, ok, rot = decode(gg)
    tag = "" if idv is None else (f"-> ID {idv}  CRC {'OK' if ok else 'BAD'}" + (f"  (norm rot {rot*90})" if rot is not None else ""))
    hit = "   <==== reads as 48!" if (idv==48 and ok) else ("   <-- reflection" if name.startswith(("flip","transpose","anti")) else "")
    print(f"  {name:26s} {('DECODE FAIL' if idv is None else tag)}{hit}")

print()
# Direct check: is 48 a reflection of 7 (and is the pair symmetric)?
def reflects_to(a, b):
    ga = encode(a); res=set()
    for name, gg in named_orientations(ga):
        idv, ok, _ = decode(gg)
        if idv is not None and ok: res.add(idv)
    return b in res, sorted(res)

ok7, set7 = reflects_to(7,48)
ok48, set48 = reflects_to(48,7)
print(f"valid IDs reachable from code 7's tag by any rotation/reflection: {set7}")
print(f"valid IDs reachable from code 48's tag by any rotation/reflection: {set48}")
print(f"7 can impersonate 48: {ok7}    48 can impersonate 7: {ok48}")
