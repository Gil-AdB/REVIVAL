import re,sys
path=sys.argv[1]
txt=open(path,errors='replace').read()
m=re.search(r'^Sort by top of stack.*?\n(.*?)\n\n', txt, re.S|re.M)
body=m.group(1)
rows=[]
for line in body.splitlines():
    line=line.strip()
    if not line: continue
    mm=re.match(r'^(.*?)\s+(\d+)$', line)
    if not mm: continue
    rows.append((mm.group(1).strip(), int(mm.group(2))))
demo=[(n,c) for n,c in rows if n.find('(in DEMO')>=0]
tot=sum(c for _,c in demo)
print("DEMO self samples total: %d   (all-symbol total %d)"%(tot,sum(c for _,c in rows)))
pats=sys.argv[2:] or ['FrustumClipper::Render','MiplevelClipper','FaceTileBins_Build','RenderInnerMekalele','RenderInner','apply_exact<false>','Render_VolumetricCones_Tile','Calc_Flags','YSort']
for p in pats:
    s=sum(c for n,c in demo if p in n)
    if s: print("  %-60s %8d  %6.3f %%"%(p,s,100.0*s/tot))
    else: print("  %-60s        0   0.000 %%"%p)
