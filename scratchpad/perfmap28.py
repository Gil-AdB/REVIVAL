#!/usr/bin/env python3
"""2026-08-28 ranked-cost-map battery.

Interleaved, order-rotated, min-of-rounds runner over the user's three real
arms.  One binary, one asset tree, dummy SDL drivers, --profiler=0.
Emits a JSON blob of {arm: {row: [per-round values]}} for the report step.
"""
import json, os, re, subprocess, sys, time, collections

RT = "/Users/gil-ad/work/rev-perfmap/Runtime"
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

HIS_GREETS = ["--deferred", "--hdr", "--hdr-linear", "--texture-filter=2",
              "--ssao", "--ssao-gtao", "--greets-displace"]
HIS_CITY   = ["--env_live_water", "--deferred", "--city-env-pixel"]
HIS_CHASE  = ["--deferred", "--hdr", "--hdr-linear", "--texture-filter=2",
              "--ssao", "--ssao-gtao"]

ARMS = [
    ("greets_t5743", "greets", 5743, HIS_GREETS),
    ("greets_t5965", "greets", 5965, HIS_GREETS),
    ("city_t1961",   "city",   1961, HIS_CITY),
    ("chase_t1105",  "chase",  1105, HIS_CHASE),
    ("chase_t800",   "chase",   800, HIS_CHASE),
]

ITERS  = int(os.environ.get("PM_ITERS", 24))
ROUNDS = int(os.environ.get("PM_ROUNDS", 12))
HW     = os.environ.get("PM_HW", "0") == "1"
ONLY   = os.environ.get("PM_ONLY", "")
BIN    = os.environ.get("PM_BIN", "DEMO")
OUT    = os.environ.get("PM_OUT", "/Users/gil-ad/work/rev-perfmap/scratchpad/perfmap28.json")

if ONLY:
    keep = set(ONLY.split(","))
    ARMS = [a for a in ARMS if a[0] in keep]

# [DPROF]   gbuffer                 2.00     7.287     7.544    50.795     6.7 | ...
ROW = re.compile(r"^\[DPROF\] (\s*)(\S.*?)\s\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)")
BEN = re.compile(r"^\[BENCH\] scene=\S+ t=\d+ iters=\d+ total=([\d.]+) ms\s+mean=([\d.]+)")

def run(scene, t, flags):
    cmd = ([f"{RT}/{BIN}", f"--bench=scene@scene={scene},t={t},iters={ITERS}"]
           + flags + ["--profiler=0", "--deferred_prof=1", "--strict_flags"])
    if HW: cmd.append("--hw_prof")
    p = subprocess.run(cmd, cwd=RT, env=ENV, capture_output=True, text=True, timeout=900)
    txt = p.stdout + p.stderr
    out = {}
    for line in txt.splitlines():
        m = BEN.match(line)
        if m:
            out["__tick_mean"] = float(m.group(2))
            continue
        m = ROW.match(line)
        if not m: continue
        depth = len(m.group(1)) // 2
        name = m.group(2).strip()
        if name.startswith("=") or name == "phase": continue
        key = name
        try:
            wmin, wavg = float(m.group(4)), float(m.group(5))
        except ValueError:
            continue
        try: calls = float(m.group(3))
        except ValueError: calls = None
        try: thr = float(m.group(6))
        except ValueError: thr = None
        try: par = float(m.group(7))
        except ValueError: par = None
        out[key] = dict(calls=calls, wall_min=wmin, wall_avg=wavg,
                        thrsum=thr, effPar=par, depth=depth)
        if HW:
            g = re.search(r"\|\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", line)
            if g:
                out[key]["Ginstr"] = float(g.group(1))
                out[key]["Gcyc"]   = float(g.group(2))
                out[key]["IPC"]    = float(g.group(3))
    if "__tick_mean" not in out:
        sys.stderr.write("!! no BENCH line for %s t=%d\n%s\n" % (scene, t, txt[-2000:]))
    return out

acc = collections.defaultdict(list)
t0 = time.time()
for r in range(ROUNDS):
    order = ARMS[r % len(ARMS):] + ARMS[:r % len(ARMS)]     # rotate every round
    for name, scene, t, flags in order:
        acc[name].append(run(scene, t, flags))
    sys.stderr.write("round %d/%d done (%.0fs, load %s)\n" %
                     (r + 1, ROUNDS, time.time() - t0, os.getloadavg()[0]))

json.dump({k: v for k, v in acc.items()}, open(OUT, "w"))
sys.stderr.write("wrote %s\n" % OUT)
