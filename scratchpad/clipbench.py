import json,os,re,subprocess,collections
RT="/Users/gil-ad/work/rev-chaseperf/Runtime"; ABL="/Users/gil-ad/work/rev-chaseperf/scratchpad/abl"
ENV=dict(os.environ,SDL_VIDEODRIVER="dummy",SDL_AUDIODRIVER="dummy")
HIS=["--deferred","--hdr","--hdr-linear","--texture-filter=2","--ssao","--ssao-gtao"]
ROW=re.compile(r"^\[DPROF\] (\s*)(\S.*?)\s\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)")
BEN=re.compile(r"^\[BENCH\].*mean=([\d.]+)")
ARMS={"noclip":("DEMO_noclip",[]), "clip":("DEMO_clip",[]),
      "noclip_b":("DEMO_noclip",["--water_glints_batch"]),
      "clip_b":("DEMO_clip",["--water_glints_batch"]),
      "noclip2":("DEMO_noclip",[])}
def run(t,a):
    exe,fl=ARMS[a]
    p=subprocess.run([f"{ABL}/{exe}",f"--bench=scene@scene=chase,t={t},iters=20"]+HIS+fl+
                     ["--profiler=0","--deferred_prof=1","--strict_flags"],cwd=RT,env=ENV,
                     capture_output=True,text=True,timeout=1800)
    o={}
    for l in (p.stdout+p.stderr).splitlines():
        m=BEN.match(l)
        if m: o["__tick"]=float(m.group(1)); continue
        m=ROW.match(l)
        if m:
            n=m.group(2).strip()
            if n.startswith("=") or n=="phase": continue
            try: w=float(m.group(4))
            except ValueError: continue
            if w<=0.0 and n in o: continue
            o[n]=w
    return o
d=collections.defaultdict(lambda: collections.defaultdict(list))
jobs=[(t,a) for t in (800,1105,1600) for a in ARMS]
for r in range(10):
    for (t,a) in jobs[r%len(jobs):]+jobs[:r%len(jobs)]:
        res=run(t,a)
        if r==0: continue
        for k,v in res.items(): d[f"t{t}/{a}"][k].append(v)
    print("round",r,flush=True)
json.dump({k:dict(v) for k,v in d.items()},open("clip.json","w"))
print("wrote")
