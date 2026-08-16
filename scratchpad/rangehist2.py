import re,sys,collections
path=sys.argv[1]; want=sys.argv[2]
lines=open(path,errors='replace').read().splitlines()
try: s=lines.index("Call graph:")
except ValueError: s=0
rows=[]
pat=re.compile(r'^(?P<pre>[\s!:|+]*)(?P<cnt>\d+)\s(?P<rest>.*)$')
for ln in lines[s+1:]:
    if ln.startswith("Total number in stack") or ln.startswith("Binary Images"): break
    m=pat.match(ln)
    if not m: continue
    depth=len(m.group('pre')); cnt=int(m.group('cnt')); rest=m.group('rest')
    off=None
    mo=re.search(r'\+\s([\d,\.]+)\s+\[', rest)
    if mo:
        parts=[p for p in mo.group(1).split(',') if p.isdigit()]
        off=[int(p) for p in parts]
    sym=re.sub(r'\s+\(in .*$','',rest).strip()
    rows.append([depth,cnt,sym,off,0])
st=[]
for i,r in enumerate(rows):
    while st and rows[st[-1]][0]>=r[0]: st.pop()
    if st: rows[st[-1]][4]+=r[1]
    st.append(i)
self_by=collections.defaultdict(float)
for d,cnt,sym,off,ch in rows:
    if want not in sym: continue
    self=cnt-ch
    if self<=0: continue
    if off:
        for o in off: self_by[o]+=self/len(off)
    else: self_by[-1]+=self
# DEMO-wide denominator from the leaf histogram
m=re.search(r'^Sort by top of stack.*?\n(.*?)\n\n', "\n".join(lines), re.S|re.M)
demo_tot=0
for line in m.group(1).splitlines():
    line=line.strip()
    if '(in DEMO)' not in line: continue
    mm=re.match(r'^(.*?)\s+(\d+)$', line)
    if mm: demo_tot+=int(mm.group(2))
tot=sum(self_by.values())
print("%s SELF = %.0f of %d DEMO self samples (%.3f%% of frame)"%(want,tot,demo_tot,100*tot/demo_tot))
ranges=[("prologue + stats_tls + entered++",0,100),("the 3 Vertex copies (43 instr)",100,284),
        ("UV/UZ/VZ stamp from Face",284,432),("Calc_Flags x3 + C_Flags OR",432,660),
        ("Z clip Near()/Far() (inlined)",660,2128),("2D clip L/R/U/D (inlined)",2128,3432),
        ("YSort",3432,3512),("MiplevelClipper (inlined)",3512,99999)]
for name,a,b in ranges:
    s=sum(c for o,c in self_by.items() if a<=o<b)
    print("  %-32s %7.1f  %5.1f%% of Render   %6.3f%% of frame"%(name,s,100*s/tot,100*s/demo_tot))
