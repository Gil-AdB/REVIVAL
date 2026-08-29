#!/usr/bin/env python3
import sys,re,collections
rows=collections.defaultdict(list)
for ln in open(sys.argv[1]):
    m=re.match(r'r=(\d+) arm=(\S+) mode=(\S+) (.*)',ln.strip())
    if not m: continue
    r,arm,mode,rest=m.groups()
    for part in rest.split('~'):
        part=part.strip()
        if not part: continue
        mm=re.match(r'\[DPROF\]\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|?\s*(.*)',part)
        if not mm: continue
        phase=mm.group(1); wmin=float(mm.group(3)); tail=mm.group(7).split()
        if mode=='wall':
            rows[(arm,phase,'wall_min')].append(wmin)
        else:
            if len(tail)>=3:
                try:
                    rows[(arm,phase,'Ginstr')].append(float(tail[0]))
                    rows[(arm,phase,'Gcyc')].append(float(tail[1]))
                except: pass
arms=[]
for (a,p,k) in rows:
    if a not in arms: arms.append(a)
phases=[]
for (a,p,k) in rows:
    if p not in phases: phases.append(p)
base=arms[0]
for p in phases:
    for k in ('wall_min','Gcyc','Ginstr'):
        vals={a:rows.get((a,p,k),[]) for a in arms}
        if not vals[base]: continue
        b=min(vals[base])
        spread=(max(vals[base])-b)/b*100
        out=f"{p:14s} {k:8s} | base min={b:8.4f} (n={len(vals[base])}, within-arm spread {spread:5.2f}%)"
        for a in arms[1:]:
            if not vals[a]: continue
            v=min(vals[a]); sp=(max(vals[a])-v)/v*100
            out+=f" | {a}={v:8.4f} ({100*(v-b)/b:+6.2f}%, spread {sp:5.2f}%)"
        print(out)
