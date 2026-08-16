#!/usr/bin/env python3
"""2026-08-16x --needle_cull ladder report: min-of-rounds per arm, round 0 dropped
(first round after a fresh process/cache is the cold one). The `floor` arm is a
second copy of `off`, so floor-vs-off is the noise bar the on-vs-off delta has to
clear."""
import sys, collections

BENCH_COLS = ["frame_min_ms", "TOTL_ms", "rf_wall_ms", "rf_Ginstr", "rf_Gcyc", "RNDR_ms", "XFRM_ms"]
CHASE_COLS = ["rf_wall_ms", "rf_Ginstr", "rf_Gcyc", "gbuf_ms", "gbuf_Ginstr"]

for path in sys.argv[1:]:
    rows = collections.defaultdict(list)
    for line in open(path):
        p = line.strip().split(",")
        if len(p) < 3: continue
        try: rnd = int(p[0])
        except ValueError: continue
        if rnd == 0: continue
        vals = []
        for v in p[2:]:
            try: vals.append(float(v))
            except ValueError: vals.append(float("nan"))
        rows[p[1]].append(vals)
    if not rows:
        print(f"{path}: no data"); continue
    ncol = min(len(v[0]) for v in rows.values())
    cols = CHASE_COLS if ncol == len(CHASE_COLS) else BENCH_COLS
    mins = {arm: [min(r[i] for r in rs) for i in range(ncol)] for arm, rs in rows.items()}
    n = min(len(rs) for rs in rows.values())
    print(f"\n=== {path}  (min of {n} rounds, round 0 dropped) ===")
    print(f"{'column':<14} {'off':>10} {'on':>10} {'floor':>10} {'on-off %':>10} {'floor-off %':>12}")
    for i, c in enumerate(cols[:ncol]):
        o, on, fl = mins["off"][i], mins["on"][i], mins["floor"][i]
        d = 100.0 * (on - o) / o if o else 0.0
        f = 100.0 * (fl - o) / o if o else 0.0
        print(f"{c:<14} {o:10.3f} {on:10.3f} {fl:10.3f} {d:+10.2f} {f:+12.2f}")
