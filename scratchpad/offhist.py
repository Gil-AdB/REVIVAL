import re,sys,collections
# Parse macOS `sample` call-tree; compute SELF samples per (symbol, +offset).
path=sys.argv[1]; want=sys.argv[2]
lines=open(path,errors='replace').read().splitlines()
# tree section: starts after "Call graph:" and ends at "Total number in stack"
try: s=lines.index("Call graph:")
except ValueError: s=0
rows=[]
pat=re.compile(r'^(?P<pre>[\s!:|+]*)(?P<cnt>\d+)\s(?P<rest>.*)$')
for ln in lines[s+1:]:
    if ln.startswith("Total number in stack") or ln.startswith("Binary Images"): break
    m=pat.match(ln)
    if not m: continue
    pre=m.group('pre'); cnt=int(m.group('cnt')); rest=m.group('rest')
    depth=len(pre)
    off=None
    mo=re.search(r'\+\s([\d,\.]+)\s+\[', rest)
    if mo:
        parts=[p for p in mo.group(1).split(',') if p.isdigit()]
        off=[int(p) for p in parts]
    sym=re.sub(r'\s+\(in .*$','',rest).strip()
    rows.append([depth,cnt,sym,off,0])   # last = children sum
# children: next rows with greater depth until depth<=cur
st=[]
for i,r in enumerate(rows):
    d=r[0]
    while st and rows[st[-1]][0]>=d: st.pop()
    if st: rows[st[-1]][4]+=r[1]
    st.append(i)
self_by=collections.defaultdict(float)
tot=0.0
for d,cnt,sym,off,ch in rows:
    if want not in sym: continue
    self=cnt-ch
    if self<=0: continue
    tot+=self
    if off:
        for o in off: self_by[o]+=self/len(off)
    else: self_by[-1]+=self
print("symbol match: %s   total SELF samples: %.0f"%(want,tot))
for o,c in sorted(self_by.items(), key=lambda kv:-kv[1])[:40]:
    print("  +%-8s %8.1f  %5.1f%%"%(o,c,100*c/tot))

