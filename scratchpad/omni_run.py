#!/usr/bin/env python3
"""omni_run.py — interleaved N-arm bench runner for the omni-loop campaign.

Every arm is an explicit argv LIST (the standing "LITERAL flags" rule); arms
rotate every round so no arm owns a load ramp; round 0 is discarded.

  omni_run.py run    OUTDIR ARMS.json ROUNDS
  omni_run.py report OUTDIR [phase,phase,...]

ARMS.json = [{"tag": ..., "bin": ..., "args": [...], "env": {...}}, ...]
Raw stdout is kept per (arm, round) BEFORE parsing.
"""
import json, os, re, subprocess, sys, time
from collections import defaultdict

BASE_ENV = {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
DPROF = re.compile(
    r"^\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)"
    r"\s*\|\s+(\S+)\s+(\S+)\s+(\S+)")
FRAME = re.compile(r"frame_ms min/p50/p95/max = ([\d.]+)/")


def load():
    try:
        return subprocess.run(["uptime"], capture_output=True, text=True
                              ).stdout.split("averages:")[-1].strip()
    except Exception:
        return "?"


def run(outdir, armsfile, rounds):
    arms = json.load(open(armsfile))
    os.makedirs(outdir, exist_ok=True)
    print("# load at start: %s" % load(), flush=True)
    for r in range(rounds):
        order = arms[r % len(arms):] + arms[:r % len(arms)]
        for a in order:
            env = dict(os.environ); env.update(BASE_ENV); env.update(a.get("env", {}))
            p = os.path.join(outdir, "%s__r%d.txt" % (a["tag"], r))
            t = time.time()
            with open(p, "w") as fh:
                fh.write("# load: %s\n# argv: %s\n" %
                         (load(), json.dumps([a["bin"]] + a["args"])))
                fh.flush()
                rc = subprocess.run([a["bin"]] + a["args"], env=env, stdout=fh,
                                    stderr=subprocess.STDOUT, timeout=3600).returncode
            print("  r%d %-28s rc=%d %6.1fs" % (r, a["tag"], rc, time.time() - t),
                  flush=True)
    print("# load at end: %s" % load())


def parse(path):
    d = {"phases": {}, "frame_min": None, "load": None}
    for line in open(path, errors="replace"):
        if line.startswith("# load:"):
            d["load"] = line.split(":", 1)[1].strip()
        m = DPROF.match(line.rstrip())
        if m:
            def f(s):
                try: return float(s)
                except ValueError: return None
            d["phases"][m.group(1).strip()] = dict(
                wall_min=f(m.group(3)), thrsum=f(m.group(5)), effpar=f(m.group(6)),
                ginstr=f(m.group(7)), gcyc=f(m.group(8)), ipc=f(m.group(9)))
        m = FRAME.search(line)
        if m: d["frame_min"] = float(m.group(1))
    return d


def report(outdir, phases):
    runs = defaultdict(dict)
    for fn in sorted(os.listdir(outdir)):
        if "__r" not in fn or not fn.endswith(".txt"): continue
        tag, r = fn[:-4].split("__r")
        runs[tag][int(r)] = parse(os.path.join(outdir, fn))
    tags = list(runs)
    hdr = "%-26s %8s" % ("arm", "frame")
    for p in phases: hdr += " | %10s %7s %7s" % (p[:10], "Gi/f", "Gc/f")
    print(hdr); print("-" * len(hdr))
    for tag in tags:
        rs = [v for k, v in sorted(runs[tag].items()) if k > 0] or list(runs[tag].values())
        fm = [x["frame_min"] for x in rs if x["frame_min"]]
        line = "%-26s %8.2f" % (tag, min(fm) if fm else float("nan"))
        for p in phases:
            w = [x["phases"][p]["wall_min"] for x in rs
                 if p in x["phases"] and x["phases"][p]["wall_min"] is not None]
            g = [x["phases"][p]["ginstr"] for x in rs
                 if p in x["phases"] and x["phases"][p]["ginstr"] is not None]
            c = [x["phases"][p]["gcyc"] for x in rs
                 if p in x["phases"] and x["phases"][p]["gcyc"] is not None]
            line += " | %10.3f %7.3f %7.3f" % (min(w) if w else float("nan"),
                                               min(g) if g else float("nan"),
                                               min(c) if c else float("nan"))
        print(line)
    ld = set()
    for tag in tags:
        for v in runs[tag].values():
            if v["load"]: ld.add(v["load"].split(" ")[0])
    print("# loads seen: %s" % ", ".join(sorted(ld)))


if __name__ == "__main__":
    if sys.argv[1] == "run":
        run(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    else:
        ph = sys.argv[3].split(",") if len(sys.argv) > 3 else ["lighting-w1", "renderFrame"]
        report(sys.argv[2], ph)
