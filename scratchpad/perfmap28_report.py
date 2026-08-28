#!/usr/bin/env python3
import json, sys, collections
OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/gil-ad/work/rev-perfmap/scratchpad/perfmap28.json"
DROP = int(sys.argv[2]) if len(sys.argv) > 2 else 1     # discard first N rounds
d = json.load(open(OUT))

def stat(vals):
    v = sorted(vals)
    if not v: return None, None
    mn = v[0]
    fl = ((v[1] - v[0]) / v[0] * 100.0) if len(v) > 1 and v[0] > 0 else 0.0
    return mn, fl

for arm in ["greets_t5743", "greets_t5965", "city_t1961", "chase_t1105", "chase_t800"]:
    if arm not in d: continue
    runs = d[arm][DROP:]
    n = len(runs)
    tick, tfl = stat([r["__tick_mean"] for r in runs if "__tick_mean" in r])
    rows = {}
    for r in runs:
        for k, v in r.items():
            if k.startswith("__"): continue
            rows.setdefault(k, {"wall": [], "thr": [], "par": [], "calls": v["calls"],
                                "depth": v["depth"]})
            rows[k]["wall"].append(v["wall_min"])
            if v["thrsum"] is not None: rows[k]["thr"].append(v["thrsum"])
            if v["effPar"] is not None: rows[k]["par"].append(v["effPar"])
    rf, rffl = stat(rows["renderFrame"]["wall"]) if "renderFrame" in rows else (None, None)
    print("\n===== %s   (min of %d rounds, round(s) 0..%d discarded) =====" % (arm, n, DROP-1))
    print("whole-tick  mean ms/iter : %8.3f   floor %+5.2f%%" % (tick, tfl))
    print("renderFrame wall_min     : %8.3f   floor %+5.2f%%" % (rf, rffl))
    outside = [k for k, v in rows.items() if v["depth"] == 0 and k not in ("renderFrame", "Render-3D")]
    outsum = sum(stat(rows[k]["wall"])[0] for k in outside)
    print("outside-renderFrame DPROF: %8.3f  (%s)" % (outsum, ", ".join(sorted(outside))))
    print("UNACCOUNTED (tick - rF - outside): %7.3f  = %5.1f%% of tick" %
          (tick - rf - outsum, (tick - rf - outsum) / tick * 100.0))
    print()
    print("%-24s %5s %9s %7s %9s %6s %7s" %
          ("row", "d", "wall_min", "floor%", "core-ms", "effPar", "%rF"))
    order = sorted([k for k in rows if k != "renderFrame"],
                   key=lambda k: -stat(rows[k]["wall"])[0])
    for k in order:
        mn, fl = stat(rows[k]["wall"])
        if mn < 0.0005: continue
        th, _ = stat(rows[k]["thr"]) if rows[k]["thr"] else (None, None)
        pr, _ = stat(rows[k]["par"]) if rows[k]["par"] else (None, None)
        print("%-24s %5d %9.3f %+6.2f%% %9s %6s %6.1f%%" %
              (k, rows[k]["depth"] if k not in outside else 3, mn, fl,
               ("%.1f" % th) if th else "-", ("%.1f" % pr) if pr else "-",
               mn / rf * 100.0))
