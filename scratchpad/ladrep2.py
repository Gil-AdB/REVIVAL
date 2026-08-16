import sys, collections, statistics
cols=["framemin","totl","rf_wall","gbuf_wall","gbuf_thr","rf_Ginstr","gbuf_Ginstr","rf_Gcyc","gbuf_Gcyc"]
data=collections.defaultdict(lambda: collections.defaultdict(list))
for ln in open(sys.argv[1]):
    p=ln.strip().split(',')
    if len(p) < 2+len(cols): continue
    try: r=int(p[0])
    except: continue
    if r==0: continue
    b=p[1]
    for i,c in enumerate(cols):
        try: data[b][c].append(float(p[2+i]))
        except: pass
bins=list(data.keys())
base=bins[0]
def mn(b,c): return min(data[b][c])
print("min-of-%d, round 0 dropped, arm order rotated per round"%len(data[base][cols[0]]))
print("%-12s "%"metric" + " ".join("%12s"%b for b in bins) + "   noise floor (max over arms of (2nd-min - min)/min)")
for c in cols:
    floors=[]
    for b in bins:
        v=sorted(data[b][c])
        if len(v)>=2 and v[0]: floors.append(100.0*(v[1]-v[0])/v[0])
    nf=max(floors) if floors else float('nan')
    row=" ".join("%12.4f"%mn(b,c) for b in bins)
    print("%-12s %s   %+.2f%%"%(c,row,nf))
print()
print("%-12s "%"delta vs "+base + " ".join("%12s"%b for b in bins[1:]))
for c in cols:
    b0=mn(base,c)
    print("%-12s "%c + " ".join("%11.2f%%"%(100.0*(mn(b,c)-b0)/b0 if b0 else 0) for b in bins[1:]))
