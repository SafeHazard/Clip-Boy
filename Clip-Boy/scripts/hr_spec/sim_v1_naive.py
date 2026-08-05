#!/usr/bin/env py -3
"""
HR-spec spike: does an ANCHOR/fiducial layout decode more robustly than the
current 4x4 near/far spec on a VL53L5CX 8x8, under the perturbations that break
the current one (sub-cell offset, tilt, distance/fill, depth noise)?

Models a physical tag as a unit-square heightmap sampled into 8x8 zones (each
zone = average tag height over its angular footprint + tilt + noise; zones off
the tag read background-far). Then runs two decoders over identical renders and
reports CORRECT / MISREAD(wrong ID, the dangerous case) / NODECODE(safe reject).
"""
import random, math

SS = 5  # supersamples per zone per axis

# ---------------------------------------------------------------- current 4x4
GN = 4
ORIENT = {(0,0):1,(0,1):1,(1,0):1,(1,1):0}
ID_CELLS = [(0,2),(0,3),(1,2),(1,3),(2,0),(2,1),(2,2),(2,3)]
CRC_CELLS = [(3,0),(3,1),(3,2),(3,3)]
crc4 = lambda x:(x^(x>>4))&0xF
def enc4(i):
    g=[[0]*4 for _ in range(4)]
    for (r,c),v in ORIENT.items(): g[r][c]=v
    for k,(r,c) in enumerate(ID_CELLS): g[r][c]=(i>>k)&1
    cc=crc4(i)
    for k,(r,c) in enumerate(CRC_CELLS): g[r][c]=(cc>>k)&1
    return g
def rot4(g): return [[g[3-r][c] for r in range(4)] for c in range(4)]
def dec4(g):
    for _ in range(4):
        if all(g[r][c]==v for (r,c),v in ORIENT.items()):
            idv=sum((g[r][c]&1)<<k for k,(r,c) in enumerate(ID_CELLS))
            cr =sum((g[r][c]&1)<<k for k,(r,c) in enumerate(CRC_CELLS))
            return idv if cr==crc4(idv) else None
        g=rot4(g)
    return None

# ------------------------------------------------- anchor candidate "A3-D12"
# 3 corner anchors (always-near 1.25-cell squares at TL,TR,BL) localize an
# affine frame; a 4x4 logical data grid (minus the 3 anchor corners + 1 keypoint
# = 12 usable cells) carries 7 ID bits + 5 ECC. Hamming(12,7,?) -> we use a
# 7-bit id + 5 parity giving single-bit CORRECTION (the real win of a re-spec).
# Layout (unit square, 6x6 conceptual; anchors are the corner pads):
ANCHOR_PADS = [(0.08,0.08),(0.92,0.08),(0.08,0.92)]   # TL, TR, BL centers (unit sq)
ANCHOR_R = 0.16                                        # pad half-size (unit sq)
# 12 data cell centers laid in the interior, avoiding the anchor pads:
DATA_CENTERS = [
    (0.50,0.12),(0.72,0.30),(0.92,0.50),(0.72,0.70),
    (0.50,0.50),(0.30,0.30),(0.50,0.30),(0.70,0.50),
    (0.30,0.70),(0.50,0.70),(0.70,0.90),(0.50,0.90),
]
DATA_R = 0.075
# Hamming(12,8)-ish: 7 data + 5 parity, single-error-correcting via syndrome.
def _ham_parity(bits7):
    # positions 1..12, parity at 1,2,4,8 ; +1 overall not needed for SEC
    code=[0]*13   # 1-indexed
    dpos=[3,5,6,7,9,10,11]   # 7 data positions
    for b,p in zip(bits7,dpos): code[p]=b
    for pp in (1,2,4,8):
        s=0
        for j in range(1,13):
            if j!=pp and (j & pp): s^=code[j]
        code[pp]=s
    return code[1:13]   # 12 bits
def ham_encode(idv):
    bits7=[(idv>>i)&1 for i in range(7)]
    return _ham_parity(bits7)
def ham_decode(code12):
    code=[0]+list(code12)   # 1-indexed
    syn=0
    for pp in (1,2,4,8):
        s=0
        for j in range(1,13):
            if (j & pp): s^=code[j]
        if s: syn|=pp
    if syn and syn<=12: code[syn]^=1   # correct single-bit error
    elif syn>12: return None           # uncorrectable
    dpos=[3,5,6,7,9,10,11]
    return sum(code[p]<<i for i,p in enumerate(dpos))

def enc_anchor(idv):
    """Return list of (u,v,bit) features: anchors(bit=1) + data cells."""
    feats=[(u,v,1,'A') for (u,v) in ANCHOR_PADS]
    code=ham_encode(idv)
    for (u,v),b in zip(DATA_CENTERS, code):
        feats.append((u,v,b,'D'))
    return feats

# ---------------------------------------------------------------- renderer
def tag_height_current(u,v,grid,step,gutter):
    if not (0<=u<=1 and 0<=v<=1): return None    # off tag
    c=min(3,int(u*4)); r=min(3,int(v*4))
    if gutter>0:
        xi=u*4-c; yi=v*4-r
        if min(xi,1-xi,yi,1-yi) < gutter: return 0.0
    return step if grid[r][c] else 0.0
def tag_height_anchor(u,v,feats,step):
    if not (0<=u<=1 and 0<=v<=1): return None
    for (cu,cv,b,t) in feats:
        rr = ANCHOR_R if t=='A' else DATA_R
        if abs(u-cu)<=rr and abs(v-cv)<=rr:
            return step if b else 0.0
    return 0.0
FAR = 0.0   # flat background depth (relative)

def render(height_fn, ox, oy, fill, tilt_u, tilt_v, noise, rng):
    """8x8 zone depths (relative; near=+step). Tag square maps to
    zone coords [ox, ox+fill] x [oy, oy+fill]."""
    Z=[[None]*8 for _ in range(8)]
    valid=[[False]*8 for _ in range(8)]
    for zi in range(8):
        for zj in range(8):
            acc=0.0; n=0; ontag=0
            for si in range(SS):
                for sj in range(SS):
                    zx=zi+(si+0.5)/SS; zy=zj+(sj+0.5)/SS
                    u=(zx-ox)/fill; v=(zy-oy)/fill
                    h=height_fn(u,v)
                    if h is None: h=FAR    # background = far
                    else: ontag+=1
                    h += tilt_u*(u-0.5) + tilt_v*(v-0.5)   # tilt across tag
                    acc+=h; n+=1
            d=acc/n + rng.gauss(0,noise)
            Z[zi][zj]=d
            valid[zi][zj]= ontag>0
    return Z,valid

# ---------------------------------------------------------------- decoders
def kmeans_thresh(vals):
    lo=min(vals); hi=max(vals)
    if hi-lo<1e-9: return (lo+hi)/2
    c0,c1=lo,hi
    for _ in range(12):
        g0=[x for x in vals if abs(x-c0)<=abs(x-c1)]
        g1=[x for x in vals if abs(x-c0)> abs(x-c1)]
        if g0: c0=sum(g0)/len(g0)
        if g1: c1=sum(g1)/len(g1)
    return (c0+c1)/2

def decode_current(Z,valid):
    # bounding box of valid (on-tag) zones
    rs=[i for i in range(8) for j in range(8) if valid[i][j]]
    cs=[j for i in range(8) for j in range(8) if valid[i][j]]
    if len(rs)<12: return ('nodecode',None)
    r0,r1,c0,c1=min(rs),max(rs),min(cs),max(cs)
    h=r1-r0+1; w=c1-c0+1
    if h<3 or w<3: return ('nodecode',None)
    thr=kmeans_thresh([Z[i][j] for i in range(8) for j in range(8) if valid[i][j]])
    # sample 4x4 by averaging the zones in each quarter of the bbox (engine-style)
    g=[[0]*4 for _ in range(4)]
    for cr in range(4):
        for cc in range(4):
            zi0=r0+cr*h/4; zi1=r0+(cr+1)*h/4
            zj0=c0+cc*w/4; zj1=c0+(cc+1)*w/4
            acc=0;n=0
            for i in range(8):
                for j in range(8):
                    ci=i+0.5; cj=j+0.5
                    if zi0<=ci<zi1 and zj0<=cj<zj1 and valid[i][j]:
                        acc+=Z[i][j];n+=1
            g[r if False else cr][cc] = 1 if (n and acc/n>thr) else 0
    idv=dec4(g)
    return ('correct_or_misread',idv) if idv is not None else ('nodecode',None)

def _blob_centroid(Z,valid,uc,vc,ox,oy,fill,thr):
    # find near-zone centroid near expected anchor pos (uc,vc) in unit sq
    ezx=ox+uc*fill; ezy=oy+vc*fill
    num_i=num_j=den=0.0
    for i in range(8):
        for j in range(8):
            if not valid[i][j] or Z[i][j]<=thr: continue
            if abs((i+0.5)-ezx)<=fill*0.30 and abs((j+0.5)-ezy)<=fill*0.30:
                w=Z[i][j]-thr
                num_i+=(i+0.5)*w; num_j+=(j+0.5)*w; den+=w
    if den<=0: return None
    return (num_i/den, num_j/den)

def decode_anchor(Z,valid,step,use_ecc=True):
    allv=[Z[i][j] for i in range(8) for j in range(8) if valid[i][j]]
    if len(allv)<12: return ('nodecode',None)
    thr=kmeans_thresh(allv)
    # rough fill/offset from valid bbox to seed anchor search
    rs=[i for i in range(8) for j in range(8) if valid[i][j]]
    cs=[j for i in range(8) for j in range(8) if valid[i][j]]
    r0,r1,c0,c1=min(rs),max(rs),min(cs),max(cs)
    fill=((r1-r0+1)+(c1-c0+1))/2.0; ox=r0; oy=c0
    A=[]
    for (uc,vc) in ANCHOR_PADS:
        ctr=_blob_centroid(Z,valid,uc,vc,ox,oy,fill,thr)
        if ctr is None: return ('nodecode',None)   # anchor missing -> reject (safe)
    # solve affine: unit-sq corners (0,0)=TL,(1,0)=TR,(0,1)=BL -> zone centroids
    pts=[_blob_centroid(Z,valid,*ap,ox,oy,fill,thr) for ap in ANCHOR_PADS]
    (a0i,a0j),(a1i,a1j),(a2i,a2j)=pts
    # map (u,v)-> zone via TL + u*(TR-TL) + v*(BL-TL), but ANCHOR_PADS are inset
    # at 0.08, so derive the basis from pad spacing (0.84 unit between pads).
    span=0.92-0.08
    ei=( (a1i-a0i)/span, (a2i-a0i)/span )   # d(zone_i)/du , /dv
    ej=( (a1j-a0j)/span, (a2j-a0j)/span )
    base_i=a0i - 0.08*ei[0] - 0.08*ei[1]
    base_j=a0j - 0.08*ej[0] - 0.08*ej[1]
    def sample(u,v):
        zi=base_i+u*ei[0]+v*ei[1]; zj=base_j+u*ej[0]+v*ej[1]
        ii=int(round(zi-0.5)); jj=int(round(zj-0.5))
        if 0<=ii<8 and 0<=jj<8 and valid[ii][jj]: return Z[ii][jj]
        return None
    bits=[]
    for (u,v) in DATA_CENTERS:
        s=sample(u,v)
        if s is None: return ('nodecode',None)
        bits.append(1 if s>thr else 0)
    if use_ecc:
        idv=ham_decode(bits)
    else:
        # treat first 7 as id, last 5 as parity-check only (detection)
        dpos=[2,4,5,6,8,9,10]
        idv=sum(bits[p]<<i for i,p in enumerate(dpos)) if True else None
        # verify parity matches (detection-only variant)
        if ham_decode(bits) is None: idv=None
    if idv is None or idv>127: return ('nodecode',None)
    return ('correct_or_misread',idv)

# ---------------------------------------------------------------- monte carlo
def trial(spec, idv, rng):
    ox=rng.uniform(-0.6,0.6); oy=rng.uniform(-0.6,0.6)
    fill=rng.uniform(6.0,8.0)
    tmag=rng.uniform(0,0.45)*( -1 if rng.random()<0.5 else 1)
    tu=tmag*rng.random(); tv=tmag*(1-rng.random())
    noise=rng.uniform(0.04,0.12)   # frac of step
    step=1.0
    if spec=='current':
        grid=enc4(idv)
        hf=lambda u,v: tag_height_current(u,v,grid,step,gutter=0.13)
        Z,va=render(hf,ox,oy,fill,tu*step,tv*step,noise*step,rng)
        return decode_current(Z,va)
    else:
        feats=enc_anchor(idv)
        hf=lambda u,v: tag_height_anchor(u,v,feats,step)
        Z,va=render(hf,ox,oy,fill,tu*step,tv*step,noise*step,rng)
        return decode_anchor(Z,va,step,use_ecc=(spec=='anchor'))

def run(spec, ids, n_per, seed=1):
    rng=random.Random(seed)
    correct=misread=nod=0
    for idv in ids:
        for _ in range(n_per):
            kind,got=trial(spec,idv,rng)
            if kind=='nodecode': nod+=1
            elif got==idv: correct+=1
            else: misread+=1
    tot=correct+misread+nod
    return correct/tot, misread/tot, nod/tot

IDS=[1,2,3,5,7,16,42,48,50,55,90,95]   # mix of fragile (7,55,42) + robust (16,48)
N=120
print(f"Monte Carlo: {len(IDS)} IDs x {N} trials each = {len(IDS)*N} per spec")
print(f"perturbations: offset +-0.6 zone, fill 6-8 zones, tilt up to 0.45*step, noise 4-12% step\n")
print(f"{'spec':10s} {'CORRECT':>9} {'MISREAD':>9} {'NODECODE':>9}   <- MISREAD = confident WRONG unlock (the danger)")
for spec in ('current','anchor_noecc' if False else 'anchor'):
    c,m,nd=run(spec,IDS,N)
    print(f"{spec:10s} {c*100:8.1f}% {m*100:8.1f}% {nd*100:8.1f}%")
# also run anchor without ECC to separate pose-benefit from ECC-benefit
c,m,nd=run('anchor_noecc',IDS,N)
print(f"{'anc(noECC)':10s} {c*100:8.1f}% {m*100:8.1f}% {nd*100:8.1f}%")
