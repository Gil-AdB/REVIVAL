import json,os,re,subprocess,sys,time,collections
RT="/Users/gil-ad/work/rev-perfmap/Runtime"
ENV=dict(os.environ,SDL_VIDEODRIVER="dummy",SDL_AUDIODRIVER="dummy")
G=["--deferred","--hdr","--hdr-linear","--texture-filter=2","--ssao","--ssao-gtao","--greets-displace"]
C=["--env_live_water","--deferred","--city-env-pixel"]
BEN=re.compile(r"mean=([\d.]+)")
ROW=re.compile(r"^\[DPROF\] (\s*)(\S.*?)\s\s+(\S+)\s+([\d.]+)\s+([\d.]+)")
arms=[("new_greets","DEMO","greets",5743,G),("ctl_greets","DEMO_ctl","greets",5743,G),
      ("new_city","DEMO","city",1961,C),("ctl_city","DEMO_ctl","city",1961,C)]
acc=collections.defaultdict(list)
for r in range(7):
    order=arms[r%len(arms):]+arms[:r%len(arms)]
    for n,b,s,t,f in order:
        p=subprocess.run([f"{RT}/{b}",f"--bench=scene@scene={s},t={t},iters=24"]+f+
                         ["--profiler=0","--deferred_prof=1","--strict_flags"],
                         cwd=RT,env=ENV,capture_output=True,text=True,timeout=900)
        txt=p.stdout+p.stderr; d={}
        for line in txt.splitlines():
            m=BEN.search(line)
            if line.startswith("[BENCH]") and m: d["__tick"]=float(m.group(1))
            m2=ROW.match(line)
            if m2:
                try: d[m2.group(2).strip()]=float(m2.group(4))
                except ValueError: pass
        acc[n].append(d)
    sys.stderr.write("round %d load %.1f\n"%(r+1,os.getloadavg()[0]))
def mn(a,k):
    xs=sorted([x[k] for x in acc[a][1:] if k in x]); return (xs[0],(xs[1]-xs[0])/xs[0]*100) if xs else (None,0)
print("%-22s %10s %10s %10s %10s"%("row","new_greets","ctl_greets","new_city","ctl_city"))
for k in ["__tick","renderFrame","gbuffer","ssao","lighting-w1","cones","fastfog","TBR-render"]:
    row="%-22s"%k
    for a in ["new_greets","ctl_greets","new_city","ctl_city"]:
        v,fl=mn(a,k); row+="%10s"%(("%.3f"%v) if v is not None else "-")
    print(row)
print()
for pair in [("new_greets","ctl_greets"),("new_city","ctl_city")]:
    for k in ["__tick","renderFrame"]:
        a,fa=mn(pair[0],k); b,fb=mn(pair[1],k)
        if a and b: print("%s %s: new %.3f (floor %+.2f%%)  ctl %.3f (floor %+.2f%%)  new/ctl %+.2f%%"%(
            pair[0].split('_')[1],k,a,fa,b,fb,(a-b)/b*100))
