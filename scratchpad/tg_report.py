#!/usr/bin/env python3
"""tg_report.py OUTDIR [phase ...] — min-of-round table for a prof1.py run dir.

Exists because prof1.py's DPROF regex assumes the `--hw_prof` column set (3
counter columns after the `|`). A wall-only arm (`--deferred_prof=1` with NO
`--hw_prof`, which is what you want when the WALL is the number under test —
counters perturb it) prints only 2, and every row silently parses as absent.
This parser splits on the `|` instead of counting columns, so it reads both.

Reports, per arm: `frame_ms min` (the bench frame floor), and per phase the
wall_min / thrsum / effPar, each min-over-rounds with round 0 dropped.
"""
import json, os, re, sys
from collections import defaultdict

FRAME = re.compile(r"frame_ms min/p50/p95/max = ([\d.]+)/")
BENCH = re.compile(r"^\[BENCH\].*mean=([\d.]+)")


def num(x):
    return None if x in ("-", "") else float(x)


def parse(path):
    d = {"phases": {}, "frame_min": None, "bench_mean": None, "load": None}
    for line in open(path, errors="replace"):
        if line.startswith("# load:"):
            d["load"] = line.split(":", 1)[1].strip()
        if line.startswith("[DPROF]"):
            body = line[len("[DPROF]"):].rstrip("\n")
            # The counter columns after `|` are present only under --hw_prof,
            # and the separator itself is dropped entirely on some rows — so
            # take whatever precedes it when it is there, the whole row when
            # it is not, and count the five wall/thread columns from the RIGHT.
            left = body.split("|")[0] if "|" in body else body
            if "(" in left:            # the OTHER (unattributed) trailer
                left = left.split("(")[0]
            toks = left.split()
            if len(toks) < 6:
                continue
            # last 5 tokens are calls, wall_min, wall_avg, thrsum, effPar
            tail, name = toks[-5:], " ".join(toks[:-5])
            if not name or name.startswith("phase"):
                continue
            try:
                d["phases"][name] = dict(calls=num(tail[0]), wall=num(tail[1]),
                                         thrsum=num(tail[3]), effpar=num(tail[4]))
            except ValueError:
                continue
            continue
        m = FRAME.search(line)
        if m:
            d["frame_min"] = float(m.group(1)); continue
        m = BENCH.match(line)
        if m:
            d["bench_mean"] = float(m.group(1))
    return d


def main():
    outdir = sys.argv[1]
    want = sys.argv[2:] or ["renderFrame", "gbuffer"]
    meta = json.load(open(os.path.join(outdir, "_meta.json")))
    tags = [a["tag"] for a in meta["arms"]]
    runs = defaultdict(list)
    for f in sorted(os.listdir(outdir)):
        m = re.match(r"(.*)__r(\d+)\.txt$", f)
        if not m or int(m.group(2)) == 0:
            continue
        runs[m.group(1)].append(parse(os.path.join(outdir, f)))

    print("# %s   rounds=%d (r0 dropped)   load %s -> %s"
          % (outdir, meta["rounds"], meta["load_start"], meta["load_end"]))
    w = max(len(n) for n in want + ["frame_ms min", "bench mean"]) + 2
    hdr = "%-*s" % (w, "row") + "".join("%13s" % t[:13] for t in tags)
    print(hdr)
    for key, lab in (("frame_min", "frame_ms min"), ("bench_mean", "bench mean")):
        row = "%-*s" % (w, lab)
        for t in tags:
            v = [x[key] for x in runs.get(t, []) if x[key] is not None]
            row += "%13s" % ("%.3f" % min(v) if v else "-")
        print(row)
    print("-" * len(hdr))
    for col, lab in (("wall", "wall_min ms"), ("thrsum", "thrsum ms"), ("effpar", "effPar")):
        print("== %s ==" % lab)
        for n in want:
            row = "%-*s" % (w, "  " + n)
            for t in tags:
                v = [x["phases"][n][col] for x in runs.get(t, [])
                     if n in x["phases"] and x["phases"][n][col] is not None]
                if not v:
                    row += "%13s" % "-"
                else:
                    row += "%13s" % ("%.3f" % (max(v) if col == "effpar" else min(v)))
            print(row)


main()
