#!/usr/bin/env py -3
"""HR-spec spike v2 — fair fight, proper sizes.
Current 4x4 (blind quarter-sampling + global threshold + CRC) vs an anchor
candidate: 4x4 lattice, 3 corner anchors (TL,TR,BL) for affine pose, the other
13 cells carry data (8 ID + 4 CRC = same payload as current, so the ONLY
difference is pose-corrected center sampling + anchor-relative threshold).
Both ~2-zone cells. Metric: CORRECT / MISREAD(confident wrong) / NODECODE(safe).
"""
import random
SS=5
GN=4
ORIENT={(0,0):1,(0,1):1,(1,0):1,(1,1):0}
ID_CELLS=[(0,2),(0,3),(1,2),(1,3),(2,0),(2,1),(2,2),(2,3)]
CRC_CELLS=[(3,0),(3,1),(3,2),(3,3)]
crc4=lambda x:(x^(x>>4))&0xF
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

# anchor layout: 4x4 lattice. (r,c). u=col axis, v=row axis. cell center:
def cc_uv(r,c): return ((c+0.5)/4.0,(r+0.5)/4.0)
ANCH=[(0,0),(0,3),(3,0)]                 # TL,TR,BL near pads (asymmetric -> rotation fixed)
DCELLS=[(r,c) for r in range(4) for c in range(4) if (r,c) not in ANCH][:12]
CELL_HALF=0.105                          # square half-width (~1.7 zones at fill 8)
def enc_anchor(idv):
    feats=[(cc_uv(r,c)[0],cc_uv(r,c)[1],1) for (r,c) in ANCH]
    cc=crc4(idv&0xFF)
    seq=[(idv>>i)&1 for i in range(8)]+[(cc>>i)&1 for i in range(4)]
    for (r,c),b in zip(DCELLS,seq):
        u,v=cc_uv(r,c); feats.append((u,v,b))
    return feats

def h_current(u,v,grid,step,gut):
    if not(0<=u<=1 and 0<=v<=1): return None
    c=min(3,int(u*4)); r=min(3,int(v*4))
    if gut>0:
        xi=u*4-c; yi=v*4-r
        if min(xi,1-xi,yi,1-yi)<gut: return 0.0
    return step if grid[r][c] else 0.0
def h_anchor(u,v,feats,step):
    if not(0<=u<=1 and 0<=v<=1): return None
    for (cu,cv,b) in feats:
        if abs(u-cu)<=CELL_HALF and abs(v-cv)<=CELL_HALF:
            return step if b else 0.0
    return 0.0

def render(hf,ox,oy,fill,tu,tv,noise,rng):
    Z=[[0.0]*8 for _ in range(8)]; va=[[False]*8 for _ in range(8)]
    for i in range(8):
        for j in range(8):
            acc=0.0;n=0;on=0
            for si in range(SS):
                for sj in range(SS):
                    zx=i+(si+0.5)/SS; zy=j+(sj+0.5)/SS
                    u=(zx-ox)/fill; v=(zy-oy)/fill
                    h=hf(u,v)
                    if h is None: h=0.0
                    else: on+=1
                    h+=tu*(u-0.5)+tv*(v-0.5)
                    acc+=h;n+=1
            Z[i][j]=acc/n+rng.gauss(0,noise); va[i][j]=on>0
    return Z,va
def kthr(vals):
    lo,hi=min(vals),max(vals)
    if hi-lo<1e-9: return (lo+hi)/2
    c0,c1=lo,hi
    for _ in range(12):
        g0=[x for x in vals if abs(x-c0)<=abs(x-c1)]; g1=[x for x in vals if abs(x-c0)>abs(x-c1)]
        if g0:c0=sum(g0)/len(g0)
        if g1:c1=sum(g1)/len(g1)
    return (c0+c1)/2

def decode_current(Z,va):
    rs=[i for i in range(8) for j in range(8) if va[i][j]]
    cs=[j for i in range(8) for j in range(8) if va[i][j]]
    if len(rs)<12: return None
    r0,r1,c0,c1=min(rs),max(rs),min(cs),max(cs)
    h=r1-r0+1;w=c1-c0+1
    if h<3 or w<3: return None
    thr=kthr([Z[i][j] for i in range(8) for j in range(8) if va[i][j]])
    g=[[0]*4 for _ in range(4)]
    for cr in range(4):
        for ccx in range(4):
            zi0=r0+cr*h/4;zi1=r0+(cr+1)*h/4;zj0=c0+ccx*w/4;zj1=c0+(ccx+1)*w/4
            acc=0;n=0
            for i in range(8):
                for j in range(8):
                    if zi0<=i+0.5<zi1 and zj0<=j+0.5<zj1 and va[i][j]: acc+=Z[i][j];n+=1
            g[cr][ccx]=1 if (n and acc/n>thr) else 0
    return dec4(g)

def _centroid(Z,va,ezx,ezy,rad,thr):
    ni=nj=den=0.0
    for i in range(8):
        for j in range(8):
            if not va[i][j] or Z[i][j]<=thr: continue
            if abs(i+0.5-ezx)<=rad and abs(j+0.5-ezy)<=rad:
                w=Z[i][j]-thr; ni+=(i+0.5)*w; nj+=(j+0.5)*w; den+=w
    return (ni/den,nj/den) if den>0 else None
def _affine(uv,zz):
    # solve a,b,c,d,e,f : zi=a*u+b*v+c ; zj=d*u+e*v+f from 3 correspondences
    (u0,v0),(u1,v1),(u2,v2)=uv; (i0,j0),(i1,j1),(i2,j2)=zz
    det=(u1-u0)*(v2-v0)-(u2-u0)*(v1-v0)
    if abs(det)<1e-9: return None
    def solve(f0,f1,f2):
        a=((f1-f0)*(v2-v0)-(f2-f0)*(v1-v0))/det
        b=((u1-u0)*(f2-f0)-(u2-u0)*(f1-f0))/det
        c=f0-a*u0-b*v0
        return a,b,c
    return solve(i0,i1,i2),solve(j0,j1,j2)
def decode_anchor(Z,va,step):
    allv=[Z[i][j] for i in range(8) for j in range(8) if va[i][j]]
    if len(allv)<12: return None
    thr=kthr(allv)
    rs=[i for i in range(8) for j in range(8) if va[i][j]]; cs=[j for i in range(8) for j in range(8) if va[i][j]]
    r0,r1,c0,c1=min(rs),max(rs),min(cs),max(cs)
    fill=((r1-r0+1)+(c1-c0+1))/2.0; ox=r0; oy=c0
    zz=[]
    for (r,c) in ANCH:
        u,v=cc_uv(r,c); ez=ox+u*fill; ey=oy+v*fill
        ct=_centroid(Z,va,ez,ey,fill*0.28,thr)
        if ct is None: return None          # missing anchor -> safe reject
        zz.append(ct)
    uv=[cc_uv(*a) for a in ANCH]
    aff=_affine(uv,zz)
    if aff is None: return None
    (ai,bi,ci),(aj,bj,cj)=aff
    bits=[]
    for (r,c) in DCELLS:
        u,v=cc_uv(r,c)
        # average the zones whose centers fall inside the cell's pose-mapped
        # footprint (uses the anchor pose to integrate over the cell area)
        acc=0.0;n=0
        for du in (-CELL_HALF*0.6,0,CELL_HALF*0.6):
            for dv in (-CELL_HALF*0.6,0,CELL_HALF*0.6):
                zi=ai*(u+du)+bi*(v+dv)+ci; zj=aj*(u+du)+bj*(v+dv)+cj
                ii=int(round(zi-0.5)); jj=int(round(zj-0.5))
                if 0<=ii<8 and 0<=jj<8 and va[ii][jj]: acc+=Z[ii][jj];n+=1
        if n==0: return None
        bits.append(1 if acc/n>thr else 0)
    idv=sum(bits[i]<<i for i in range(8)); cc=sum(bits[8+i]<<i for i in range(4))
    return idv if cc==crc4(idv) else None

def trial(spec,idv,rng):
    ox=rng.uniform(-0.5,0.5);oy=rng.uniform(-0.5,0.5);fill=rng.uniform(6.5,8.0)
    tmag=rng.uniform(0,0.20)*(1 if rng.random()<0.5 else -1)
    tu=tmag*rng.random();tv=tmag*(1-rng.random());noise=rng.uniform(0.05,0.15);step=1.0
    if spec=='current':
        g=enc4(idv); hf=lambda u,v:h_current(u,v,g,step,0.13)
        Z,va=render(hf,ox,oy,fill,tu,tv,noise,rng); return decode_current(Z,va)
    else:
        f=enc_anchor(idv); hf=lambda u,v:h_anchor(u,v,f,step)
        Z,va=render(hf,ox,oy,fill,tu,tv,noise,rng); return decode_anchor(Z,va,step)
def run(spec,ids,n,seed=7):
    rng=random.Random(seed);cor=mis=nod=0
    for idv in ids:
        for _ in range(n):
            got=trial(spec,idv,rng)
            if got is None: nod+=1
            elif got==idv: cor+=1
            else: mis+=1
    t=cor+mis+nod; return cor/t,mis/t,nod/t
IDS=[1,2,3,5,7,16,42,48,50,55,90,95]; N=200
print(f"v2: {len(IDS)} IDs x {N} = {len(IDS)*N}/spec | offset+-0.5z fill6.5-8 tilt<=0.20step noise5-15%\n")
print(f"{'spec':9s} {'CORRECT':>9} {'MISREAD':>9} {'NODECODE':>9}")
for s in ('current','anchor'):
    c,m,n=run(s,IDS,N); print(f"{s:9s} {c*100:8.1f}% {m*100:8.1f}% {n*100:8.1f}%")
