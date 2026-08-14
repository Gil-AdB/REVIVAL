#!/usr/bin/env python3
"""TIMELINE CENSUS on the DEMO's OWN authored camera (no FDS_GREETS_CAM).

The 19 review poses are hand-picked DEFECT REPROS with pinned cameras. That is a
biased sample by construction: it can only find defects at places someone already
looked. This walks the greets timeline on the camera the user actually flies and
scores void/black at every step, so a pose CLASS the review list misses shows up
as a t-range with residual.

usage: st3_timeline.py <outroot> <arm> <t0> <t1> <step> -- <flags...>
"""
import os, subprocess, sys, glob
import numpy as np

RUN = "/Users/gil-ad/work/revival-fog/Runtime"
W, H = 1920, 1080


def ppm(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3); w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3][:w*h*3], dtype=np.uint8).reshape(h, w, 3)


outroot, arm = os.path.abspath(sys.argv[1]), sys.argv[2]
t0, t1, step = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
assert sys.argv[6] == "--"
flags = sys.argv[7:]
os.makedirs(os.path.join(outroot, arm), exist_ok=True)
res = []
for t in range(t0, t1 + 1, step):
    d = os.path.join(outroot, arm, "t%05d" % t)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               FDS_SNAPSHOT_ZDUMP="1")
    env.pop("FDS_GREETS_CAM", None)          # the AUTHORED camera, on purpose
    with open(os.path.join(d, "log.txt"), "w") as lf:
        r = subprocess.run(["./DEMO", "--snapshot=greets@t=%d" % t, "--out=" + d] + flags,
                           cwd=RUN, env=env, stdout=lf, stderr=subprocess.STDOUT)
    log = open(os.path.join(d, "log.txt")).read()
    if "unknown flag" in log or "requires a value" in log:
        print("BADFLAG at t=%d" % t, flush=True); sys.exit(1)
    zs = glob.glob(os.path.join(d, "*_depth.z16")); cs = glob.glob(os.path.join(d, "*_color.ppm"))
    if not zs or not cs:
        print("t=%-5d NO DUMP (rc=%d) — past scene end?" % (t, r.returncode), flush=True)
        res.append((t, None, None)); continue
    v = int((np.fromfile(zs[0], dtype=np.uint16) == 0).sum())
    b = int((ppm(cs[0]).max(axis=2) == 0).sum())
    res.append((t, v, b))
    print("t=%-5d void=%-7d black=%-7d" % (t, v, b), flush=True)
    # keep the tree small: only retain dumps that carry residual
    if v == 0 and b == 0:
        for f in glob.glob(os.path.join(d, "*_depth.z16")) + glob.glob(os.path.join(d, "*_color.ppm")):
            os.remove(f)
tv = sum(v for _, v, _ in res if v); tb = sum(b for _, _, b in res if b)
print("TOTAL %s: void=%d black=%d over %d steps" % (arm, tv, tb, len(res)), flush=True)
