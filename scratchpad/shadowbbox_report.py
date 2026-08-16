#!/usr/bin/env python3
"""min-of-N reporter for scratchpad/shadowbbox_ladder.sh CSVs.

Round 0 is DROPPED (first run after a rebuild can write a cache the later runs
read). Per column: min, the delta vs the FIRST arm listed, and a NOISE FLOOR =
max over arms of (2nd-min - min)/min, which is the smallest move this battery
can distinguish. Print the floor next to every column or the deltas are
unreadable.
"""
import sys, collections

COLS = ["frame_min_ms", "TOTL_ms", "bake_DynOmnis_ms", "bake_DynMeshes_ms",
        "raster_DynOmnis_ms", "raster_DynMeshes_ms", "xform_DynMeshes_ms",
        "rf_wall_ms", "rf_Ginstr", "rf_Gcyc", "gbuf_Ginstr"]

def main(path):
    rows = collections.defaultdict(lambda: collections.defaultdict(list))
    order = []
    for line in open(path):
        p = line.strip().split(",")
        if len(p) < 3 or not p[0].isdigit():
            continue
        r, arm, vals = int(p[0]), p[1], p[2:]
        if r == 0:
            continue
        if arm not in order:
            order.append(arm)
        for c, v in zip(COLS, vals):
            try:
                rows[arm][c].append(float(v))
            except ValueError:
                pass
    base = order[0]
    print(f"{'column':<20} " + " ".join(f"{a:>28}" for a in order) + f" {'floor':>8} {'n':>3}")
    for c in COLS:
        floor = 0.0
        n = 0
        cells = []
        for a in order:
            v = sorted(rows[a][c])
            if len(v) < 2:
                cells.append(f"{'-':>28}")
                continue
            n = max(n, len(v))
            floor = max(floor, (v[1] - v[0]) / v[0] * 100.0)
            if a == base:
                cells.append(f"{v[0]:>28.4g}")
            else:
                b = sorted(rows[base][c])[0]
                d = (v[0] - b) / b * 100.0
                cells.append(f"{v[0]:>18.4g} ({d:+6.2f}%)")
        print(f"{c:<20} " + " ".join(cells) + f" {floor:>7.2f}% {n:>3}")

if __name__ == "__main__":
    main(sys.argv[1])
