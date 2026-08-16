#!/usr/bin/env python3
"""tg_arms.py — arm JSON for the frame raster tile-grid re-run (--frame_tile_x/y).

One binary (DEMO_tg), one asset tree, arms differ ONLY in the grid flags; plus a
parent-binary arm at the default grid, which prices the runtime-grid refactor
itself (constexpr 6x5 -> a clamped FeatureFlags read). Every arm is an explicit
argv LIST — no shell word splitting can mangle a flag.

  python3 tg_arms.py <outdir> [grids...]     grids as WxH, default 6x5 8x7 12x10
"""
import json, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
GRIDS = sys.argv[2:] or ["6x5", "8x7", "12x10"]

PROF = ["--deferred_prof=1", "--hw_prof", "--strict_flags"]

CITY_ARM = ["--env_live_water", "--deferred", "--city_env_pixel"]
GREETS_ARM = ["--deferred", "--hdr", "--hdr-linear", "--texture-filter=2",
              "--ssao", "--ssao-gtao", "--greets-displace"]
CHASE_ARM = ["--deferred"]


def grid(g):
    w, h = g.split("x")
    return ["--frame-tile-x=" + w, "--frame-tile-y=" + h]


def bench(scene, t, iters=20, xres=1512, yres=848):
    return "--bench=scene@scene=%s,t=%d,iters=%d,xres=%d,yres=%d" % (scene, t, iters, xres, yres)


def chasesnap(t, n=10):
    # chase has no --bench=scene and no --repro: the snapshot harness is asked
    # for the SAME timestamp n times; --deferred_prof drops the cold frame.
    return ["--snapshot=chase@t=" + ",".join([str(t)] * n), "--out=/tmp/tg/snap"]


def write(name, arms):
    p = os.path.join(OUT, name)
    json.dump(arms, open(p, "w"), indent=1)
    print("wrote %s (%d arms)" % (p, len(arms)))


POSES = {
    "chase_t800":  chasesnap(800) + CHASE_ARM,
    "chase_t1600": chasesnap(1600) + CHASE_ARM,
    "chase_t400":  chasesnap(400) + CHASE_ARM,
    "city_t1961":  [bench("city", 1961)] + CITY_ARM,
    "greets_t5743": [bench("greets", 5743)] + GREETS_ARM,
}

for pose, base in POSES.items():
    arms = [{"tag": "par_6x5", "args": base + PROF, "env": {},
             "bin": "./DEMO_par"}]
    for g in GRIDS:
        arms.append({"tag": "g" + g, "args": base + PROF + grid(g), "env": {},
                     "bin": "./DEMO_tg"})
    write("tg_%s.json" % pose, arms)

# Wall arms: no HW counters, no profiler overlay — the clean frame-time read.
# city/greets run under --bench, whose `frame_ms min` is the number to quote.
# chase has no --bench, so it keeps --deferred_prof=1 (walls only, no --hw_prof)
# and `renderFrame wall_min` is its frame number, exactly as PERF_STATE 00c did.
for pose, base in POSES.items():
    extra = ["--profiler=0", "--strict_flags"]
    if pose.startswith("chase"):
        extra = extra + ["--deferred_prof=1"]
    arms = [{"tag": "par_6x5", "args": base + extra, "env": {}, "bin": "./DEMO_par"}]
    for g in GRIDS:
        arms.append({"tag": "g" + g, "args": base + extra + grid(g),
                     "env": {}, "bin": "./DEMO_tg"})
    write("tgw_%s.json" % pose, arms)
