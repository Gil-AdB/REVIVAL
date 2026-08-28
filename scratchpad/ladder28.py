#!/usr/bin/env python3
"""Flag-flip ladders on ONE binary: interleaved, order-rotated, min-of-rounds."""
import json, os, re, subprocess, sys, time, collections
RT="/Users/gil-ad/work/rev-perfmap/Runtime"
ENV=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
G=["--deferred","--hdr","--hdr-linear","--texture-filter=2","--ssao","--ssao-gtao","--greets-displace"]
C=["--env_live_water","--deferred","--city-env-pixel"]
H=["--deferred","--hdr","--hdr-linear","--texture-filter=2","--ssao","--ssao-gtao"]
LADDERS={
 "L6_ssao_gtao_greets":("greets",5743,G,[("base",[]),("slices1",["--ssao_gtao_slices=1"]),
    ("steps2",["--ssao_gtao_steps=2"]),("s1st2",["--ssao_gtao_slices=1","--ssao_gtao_steps=2"]),
    ("down3",["--ssao_downscale=3"])]),
 "L7_ssao_gtao_chase":("chase",1105,H,[("base",[]),("slices1",["--ssao_gtao_slices=1"]),
    ("steps2",["--ssao_gtao_steps=2"]),("down3",["--ssao_downscale=3"]),("down4",["--ssao_downscale=4"])]),

 "L1_greets5743":("greets",5743,G,[("base",[]),("no-ssao",["--no-ssao"]),
    ("blur0",["--ssao_blur=0"]),("samp1",["--ssao_samples=1"]),("down4",["--ssao_downscale=4"])]),
 "L2_chase1105":("chase",1105,H,[("base",[]),("no-ssao",["--no-ssao"]),
    ("blur0",["--ssao_blur=0"]),("samp1",["--ssao_samples=1"])]),
 "L3_chase800":("chase",800,H,[("base",[]),("no-ssao",["--no-ssao"]),
    ("tile6x20",["--frame_tile_x=6","--frame_tile_y=20"]),
    ("tile12x10",["--frame_tile_x=12","--frame_tile_y=10"])]),
 "L4_city1961":("city",1961,C,[("base",[]),("tile6x20",["--frame_tile_x=6","--frame_tile_y=20"]),
    ("no-vlight",["--prof_no_vertex_light"]),("no-fog",["--prof_no_fog"])]),
 "L5_greets5743_tile":("greets",5743,G,[("base",[]),("tile6x20",["--frame_tile_x=6","--frame_tile_y=20"]),
    ("tile12x10",["--frame_tile_x=12","--frame_tile_y=10"])]),
}
ROW=re.compile(r"^\[DPROF\] (\s*)(\S.*?)\s\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)")
BEN=re.compile(r"^\[BENCH\] scene=\S+ t=\d+ iters=\d+ total=([\d.]+) ms\s+mean=([\d.]+)")
ITERS=int(os.environ.get("PM_ITERS",24)); ROUNDS=int(os.environ.get("PM_ROUNDS",7))
WHICH=os.environ.get("PM_LADDERS","").split(",") if os.environ.get("PM_LADDERS") else list(LADDERS)
def run(scene,t,flags):
    cmd=[f"{RT}/DEMO",f"--bench=scene@scene={scene},t={t},iters={ITERS}"]+flags+[
         "--profiler=0","--deferred_prof=1","--strict_flags"]
    p=subprocess.run(cmd,cwd=RT,env=ENV,capture_output=True,text=True,timeout=900)
    txt=p.stdout+p.stderr; out={}
    for line in txt.splitlines():
        m=BEN.match(line)
        if m: out["__tick"]=float(m.group(2)); continue
        m=ROW.match(line)
        if not m: continue
        n=m.group(2).strip()
        if n=="phase" or n.startswith("="): continue
        try: out[n]=float(m.group(4))
        except ValueError: pass
    if "__tick" not in out: sys.stderr.write("!! no BENCH %s %s\n%s\n"%(scene,flags,txt[-1500:]))
    return out
res={}
t0=time.time()
for lname in WHICH:
    scene,t,base,arms=LADDERS[lname]
    acc=collections.defaultdict(list)
    for r in range(ROUNDS):
        order=arms[r%len(arms):]+arms[:r%len(arms)]
        for an,extra in order: acc[an].append(run(scene,t,base+extra))
    res[lname]=acc
    sys.stderr.write("%s done (%.0fs, load %.1f)\n"%(lname,time.time()-t0,os.getloadavg()[0]))
json.dump(res,open(os.environ.get("PM_OUT","/Users/gil-ad/work/rev-perfmap/scratchpad/ladder28.json"),"w"))
