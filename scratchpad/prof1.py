#!/usr/bin/env python3
"""prof1.py — interleaved multi-arm profiling driver for the frame-time map (round 1).

Why python and not zsh: every arm is an explicit argv LIST, so no shell word
splitting can ever mangle a flag (the standing "LITERAL flags in scripts" rule).
Arms come from a JSON file; nothing is built by string concatenation at runtime.

  prof1.py run   OUTDIR ARMS.json ROUNDS      # interleaved round-robin, rotating order
  prof1.py report OUTDIR [--phases p1,p2,..]  # min-over-rounds table (round 0 discarded)

ARMS.json = [{"tag": "...", "args": ["--flag", ...], "env": {"FDS_X": "1"}}, ...]

Every raw run's stdout is kept in OUTDIR/<tag>__r<N>.txt BEFORE anything is
parsed (docs/HW_PROFILING.md §5: a reporting bug must cost a re-parse, never a
re-run).  Round 0 is recorded and then discarded by `report` — first-touch and
page-in noise.
"""
import json, os, re, subprocess, sys, time
from collections import defaultdict

BIN = os.environ.get("PROF1_BIN", "./DEMO")
BASE_ENV = {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}


def uptime_load():
    try:
        out = subprocess.run(["uptime"], capture_output=True, text=True).stdout
        return out.split("averages:")[-1].strip()
    except Exception:
        return "?"


def run(outdir, armsfile, rounds):
    arms = json.load(open(armsfile))
    os.makedirs(outdir, exist_ok=True)
    meta = {"bin": os.path.abspath(BIN), "arms": arms, "rounds": rounds,
            "load_start": uptime_load(), "t0": time.time()}
    for r in range(rounds):
        order = arms[r % len(arms):] + arms[:r % len(arms)]   # rotate: no arm owns a load ramp
        for a in order:
            env = dict(os.environ)
            env.update(BASE_ENV)
            env.update(a.get("env", {}))
            path = os.path.join(outdir, "%s__r%d.txt" % (a["tag"], r))
            # A per-arm "bin" is what makes a COMMIT A/B possible: two binaries,
            # one asset tree, interleaved (docs/HW_PROFILING.md §5). A one-binary
            # flag A/B prices the flag, not the commit.
            bin_ = a.get("bin", BIN)
            t = time.time()
            with open(path, "w") as fh:
                fh.write("# load: %s\n" % uptime_load())
                fh.write("# argv: %s\n" % json.dumps([bin_] + a["args"]))
                fh.write("# env:  %s\n" % json.dumps(a.get("env", {})))
                fh.flush()
                p = subprocess.run([bin_] + a["args"], env=env, stdout=fh,
                                   stderr=subprocess.STDOUT, timeout=1800)
            print("  r%d %-34s rc=%d  %6.1fs" % (r, a["tag"], p.returncode, time.time() - t),
                  flush=True)
    meta["load_end"] = uptime_load()
    meta["wall_s"] = time.time() - meta["t0"]
    json.dump(meta, open(os.path.join(outdir, "_meta.json"), "w"), indent=1)
    print("# load %s -> %s, %.0f s total" % (meta["load_start"], meta["load_end"], meta["wall_s"]))


DPROF = re.compile(r"^\[DPROF\]\s+(.*?)\s+([\d.]+|-)\s+([\d.]+|-)\s+([\d.]+|-)\s+([\d.]+|-)\s+([\d.]+|-)\s+\|\s+([\d.]+|-)\s+([\d.]+|-)\s+([\d.]+|-)\s*$")
FRAME = re.compile(r"frame_ms min/p50/p95/max = ([\d.]+)/")
SECT = re.compile(r"^(RNDR|ANIM|BAKE|LGHT|XFRM|ZCLR|SORT|FLIP|SKY|TOTL)\s+([\d.]+)\s+[\d.]+%?\s*([\d.]+)?")
BENCH = re.compile(r"^\[BENCH\] scene=\S+ t=\S+ iters=\d+ total=[\d.]+ ms\s+mean=([\d.]+)")
STONE = re.compile(r"^\[STONE\]")
INITTL = re.compile(r"^\[INIT-T\]\s+(.*)$")
SHREFL = re.compile(r"^\[SHARD-REFL\] \d+ shards / \d+ workers in ([\d.]+) ms")
SHPHASE = re.compile(r"^\[SHARD-PHASE\] core-ms setup=([\d.]+) xform=([\d.]+) raster=([\d.]+) \(gbufferfill=([\d.]+) deferredlight=([\d.]+) cones=([\d.]+)\)")


def num(x):
    return None if x == "-" else float(x)


def parse(path):
    d = {"phases": {}, "frame_min": None, "sect": {}, "bench_mean": None,
         "load": None, "stone": [], "init": [], "shard_bake": [], "shard_phase": []}
    for line in open(path, errors="replace"):
        if line.startswith("# load:"):
            d["load"] = line.split(":", 1)[1].strip()
        m = DPROF.match(line.rstrip())
        if m:
            name = m.group(1).strip()
            d["phases"][name] = dict(calls=num(m.group(2)), wall_min=num(m.group(3)),
                                     wall_avg=num(m.group(4)), thrsum=num(m.group(5)),
                                     effpar=num(m.group(6)), ginstr=num(m.group(7)),
                                     gcyc=num(m.group(8)), ipc=num(m.group(9)))
            continue
        m = FRAME.search(line)
        if m:
            d["frame_min"] = float(m.group(1))
            continue
        m = SECT.match(line.strip())
        if m and m.group(3):
            d["sect"][m.group(1)] = float(m.group(3))
            continue
        m = BENCH.match(line)
        if m:
            d["bench_mean"] = float(m.group(1))
        if STONE.match(line):
            d["stone"].append(line.strip())
        m = INITTL.match(line)
        if m:
            d["init"].append(m.group(1).strip())
        m = SHREFL.match(line)
        if m:
            d["shard_bake"].append(float(m.group(1)))
        m = SHPHASE.match(line)
        if m:
            d["shard_phase"].append([float(g) for g in m.groups()])
    return d


def report(outdir, phases=None, drop_round0=True):
    meta = json.load(open(os.path.join(outdir, "_meta.json")))
    tags = [a["tag"] for a in meta["arms"]]
    runs = defaultdict(list)
    for f in sorted(os.listdir(outdir)):
        m = re.match(r"(.*)__r(\d+)\.txt$", f)
        if not m:
            continue
        r = int(m.group(2))
        if drop_round0 and r == 0 and meta["rounds"] > 1:
            continue
        runs[m.group(1)].append(parse(os.path.join(outdir, f)))
    agg = {}
    for tag in tags:
        rs = runs.get(tag, [])
        if not rs:
            continue
        a = {"n": len(rs)}
        fm = [x["frame_min"] for x in rs if x["frame_min"]]
        a["frame_min"] = min(fm) if fm else None
        for k in ("RNDR", "ANIM", "BAKE", "XFRM", "LGHT"):
            v = [x["sect"].get(k) for x in rs if x["sect"].get(k)]
            a[k] = min(v) if v else None
        ph = defaultdict(dict)
        names = set()
        for x in rs:
            names |= set(x["phases"])
        for n in names:
            w = [x["phases"][n]["wall_min"] for x in rs if n in x["phases"] and x["phases"][n]["wall_min"] is not None]
            gi = [x["phases"][n]["ginstr"] for x in rs if n in x["phases"] and x["phases"][n]["ginstr"] is not None]
            gc = [x["phases"][n]["gcyc"] for x in rs if n in x["phases"] and x["phases"][n]["gcyc"] is not None]
            ip = [x["phases"][n]["ipc"] for x in rs if n in x["phases"] and x["phases"][n]["ipc"] is not None]
            tp = [x["phases"][n]["thrsum"] for x in rs if n in x["phases"] and x["phases"][n]["thrsum"] is not None]
            ep = [x["phases"][n]["effpar"] for x in rs if n in x["phases"] and x["phases"][n]["effpar"] is not None]
            ph[n] = dict(wall=min(w) if w else None,
                         ginstr=sum(gi) / len(gi) if gi else None,
                         gcyc=sum(gc) / len(gc) if gc else None,
                         ipc=sum(ip) / len(ip) if ip else None,
                         thrsum=min(tp) if tp else None,
                         effpar=max(ep) if ep else None)
        a["phases"] = ph
        a["loads"] = [x["load"] for x in rs]
        # shatter bracket: index 0 = cold bake frame, index 1 = the SECOND
        # shatter frame, which is the one the 11.5 ms anchor was taken on.
        for idx, lab in ((0, "cold"), (1, "2nd")):
            v = [x["shard_bake"][idx] for x in rs if len(x["shard_bake"]) > idx]
            a["shard_bake_" + lab] = min(v) if v else None
            p = [x["shard_phase"][idx] for x in rs if len(x["shard_phase"]) > idx]
            if p:
                a["shard_phase_" + lab] = [min(c[i] for c in p) for i in range(6)]
        agg[tag] = a

    order = phases.split(",") if phases else None
    if not order:
        seen = []
        for tag in tags:
            for n in agg.get(tag, {}).get("phases", {}):
                if n not in seen:
                    seen.append(n)
        order = seen
    w = max(len(n) for n in order) + 1
    print("# %s   rounds=%d (r0 dropped=%s)   load %s -> %s"
          % (outdir, meta["rounds"], drop_round0, meta["load_start"], meta["load_end"]))
    hdr = "%-*s" % (w, "phase") + "".join("%14s" % t[:14] for t in tags)
    print("\n== wall_min ms (min over rounds) ==")
    print(hdr)
    for k in ("FRAME_MIN", "RNDR", "BAKE", "ANIM"):
        key = "frame_min" if k == "FRAME_MIN" else k
        row = "%-*s" % (w, k)
        for t in tags:
            v = agg.get(t, {}).get(key)
            row += "%14s" % ("%.3f" % v if v else "-")
        print(row)
    print("-" * len(hdr))
    for n in order:
        row = "%-*s" % (w, n)
        for t in tags:
            v = agg.get(t, {}).get("phases", {}).get(n, {}).get("wall")
            row += "%14s" % ("%.3f" % v if v is not None else "-")
        print(row)
    for col, lab in (("ginstr", "Ginstr/f (mean)"), ("gcyc", "Gcyc/f (mean)"), ("ipc", "IPC (mean)")):
        print("\n== %s ==" % lab)
        print(hdr)
        for n in order:
            vals = [agg.get(t, {}).get("phases", {}).get(n, {}).get(col) for t in tags]
            if all(v is None for v in vals):
                continue
            row = "%-*s" % (w, n)
            for v in vals:
                row += "%14s" % ("%.3f" % v if v is not None else "-")
            print(row)
    print("\n== effPar (max) / thrsum (min ms) ==")
    print(hdr)
    for n in order:
        vals = [agg.get(t, {}).get("phases", {}).get(n, {}) for t in tags]
        if all(v.get("effpar") is None for v in vals):
            continue
        row = "%-*s" % (w, n)
        for v in vals:
            row += "%14s" % ("%.1f/%.0f" % (v["effpar"], v["thrsum"]) if v.get("effpar") else "-")
        print(row)
    if any(agg.get(t, {}).get("shard_bake_2nd") for t in tags):
        print("\n== shard bake, min over rounds (ms wall / core-ms) ==")
        print(hdr)
        for k in ("shard_bake_cold", "shard_bake_2nd"):
            row = "%-*s" % (w, k)
            for t in tags:
                v = agg.get(t, {}).get(k)
                row += "%14s" % ("%.1f" % v if v else "-")
            print(row)
        for i, lab in enumerate(("setup", "xform", "raster", "gbufferfill", "deferredlight", "cones")):
            row = "%-*s" % (w, "2nd:" + lab)
            for t in tags:
                v = agg.get(t, {}).get("shard_phase_2nd")
                row += "%14s" % ("%.1f" % v[i] if v else "-")
            print(row)
    return agg


if __name__ == "__main__":
    if sys.argv[1] == "run":
        run(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif sys.argv[1] == "report":
        ph = None
        if "--phases" in sys.argv:
            ph = sys.argv[sys.argv.index("--phases") + 1]
        report(sys.argv[2], ph)
