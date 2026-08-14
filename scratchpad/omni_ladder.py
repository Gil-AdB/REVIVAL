#!/usr/bin/env python3
"""omni_ladder.py — report the FDS_OMNI_ABLATE ladder written by omni_ablate.sh.

  omni_ladder.py OUTDIR [OUTDIR2 ...]     # one block per dir, then the delta table
"""
import sys, os, re, glob, collections

DPROF = re.compile(
    r"^\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)"
    r"\s*\|\s+(\S+)\s+(\S+)\s+(\S+)")

LABEL = {
    1:  "loop floor (index + bounds only)",
    2:  "+ mirrorId test",
    3:  "+ w vector, N.L dot, dot<0 reject",
    4:  "+ len2, range reject",
    5:  "+ bounce-window portal test",
    6:  "+ rsqrt/dist/attenuation k",
    7:  "+ spot cone test + smoothstep",
    8:  "+ computeMapShadowAtten (2-D maps)",
    9:  "+ resolveCubeAtten (cube tap)",
    10: "+ relief-horizon term",
    11: "+ DIFFUSE accumulate",
    0:  "FULL loop (+ specular lobe)",
}
ORDER = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0]


def collect(d, pose):
    acc = collections.defaultdict(lambda: collections.defaultdict(list))
    for f in sorted(glob.glob(os.path.join(d, "s*_%s_r*.txt" % pose))):
        b = os.path.basename(f)[:-4].split("_")
        st, run = int(b[0][1:]), int(b[-1][1:])
        if run == 1:
            continue                     # discard the first run after a rebuild
        for line in open(f):
            m = DPROF.match(line.rstrip())
            if not m:
                continue
            name = m.group(1).strip()
            if name not in ("renderFrame", "lighting-w1", "lighting-w2"):
                continue
            try:
                acc[st][name].append((float(m.group(3)), float(m.group(7)),
                                      float(m.group(8))))
            except ValueError:
                pass
    return acc


def block(d, pose):
    acc = collect(d, pose)
    if not acc:
        return None
    print("\n== %s   pose=%s ==" % (d, pose))
    print("%-4s %-38s %9s %9s %9s %10s" %
          ("st", "what is KEPT", "w1 Gi/f", "w1 Gc/f", "w1 ms", "frame Gi/f"))
    print("-" * 84)
    rows = {}
    for st in ORDER:
        if st not in acc or "lighting-w1" not in acc[st]:
            continue
        w1 = acc[st]["lighting-w1"]
        rf = acc[st].get("renderFrame", [])
        gi, gc, ms = min(x[1] for x in w1), min(x[2] for x in w1), min(x[0] for x in w1)
        fi = min(x[1] for x in rf) if rf else float("nan")
        rows[st] = (gi, gc, ms)
        print("%-4d %-38s %9.3f %9.3f %9.3f %10.3f" % (st, LABEL[st], gi, gc, ms, fi))
    # per-stage deltas
    print("\n%-4s %-38s %9s %9s %8s" % ("st", "STAGE COST (this minus previous)",
                                        "dGi/f", "dGc/f", "%% of w1"))
    print("-" * 74)
    full = rows.get(0)
    prev = None
    for st in ORDER:
        if st not in rows:
            continue
        if prev is None:
            prev = rows[st]
            print("%-4d %-38s %9.3f %9.3f %7.1f%%" %
                  (st, "(floor)", rows[st][0], rows[st][1],
                   100.0 * rows[st][0] / full[0] if full else float("nan")))
            continue
        dgi = rows[st][0] - prev[0]
        dgc = rows[st][1] - prev[1]
        print("%-4d %-38s %9.3f %9.3f %7.1f%%" %
              (st, LABEL[st], dgi, dgc,
               100.0 * dgi / full[0] if full else float("nan")))
        prev = rows[st]
    return rows


if __name__ == "__main__":
    dirs = sys.argv[1:] or ["/tmp/omniloop/ladderA"]
    per = {}
    for d in dirs:
        for pose in ("t5743", "his"):
            r = block(d, pose)
            if r:
                per[(d, pose)] = r
    # cross-session agreement
    if len(dirs) > 1:
        for pose in ("t5743", "his"):
            ds = [d for d in dirs if (d, pose) in per]
            if len(ds) < 2:
                continue
            print("\n== CROSS-SESSION AGREEMENT  pose=%s (w1 Gi/f) ==" % pose)
            print("%-4s %-38s %s %8s" % ("st", "stage", "".join("%10s" % os.path.basename(d) for d in ds), "spread%"))
            for st in ORDER:
                vs = [per[(d, pose)][st][0] for d in ds if st in per[(d, pose)]]
                if len(vs) < 2:
                    continue
                sp = 100.0 * (max(vs) - min(vs)) / max(vs)
                print("%-4d %-38s %s %7.2f%%" % (st, LABEL[st],
                                                 "".join("%10.3f" % v for v in vs), sp))
