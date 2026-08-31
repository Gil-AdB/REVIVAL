#!/usr/bin/env python3
"""Read the [V4-GRID] census and score how well a band's breaklines line up
with ANOTHER band's — the measurement that decides whether --v4_band_union's
borrowed column lines land in a block interior or on top of the host's own
mortar.

The premise --v4_band_union was built on ("a running bond puts band k+1's
mortar at the middle of band k's blocks") is a claim about exactly these
numbers, and on greets it is false: see the refutation in the ledger.

    tools/v4_grid_align.py <log with [V4-GRID] lines> [--mat rooms]

Prints, per host band, every foreign run centre with its distance to the
nearest HOST breakline and whether it falls inside the host's own recess.
"""
import re, sys, argparse

PAD = 1.25          # kStonePadTex, texels at the bake mip
MINFLOOR = 1.5      # kStoneMinFloorTex


def parse(path):
    mats = {}
    for L in open(path, errors="replace"):
        m = re.match(r"\[V4-GRID\] mat=(\S+) band=(\d+) y=\S+ vruns=(.*)$", L.strip())
        if m:
            runs = [(float(a), float(b)) for a, b, _ in
                    re.findall(r"([\d.]+)-([\d.]+)\(c([\d.]+)\)", m.group(3))]
            mats.setdefault(m.group(1), {})[int(m.group(2))] = runs
        m = re.match(r"\[V4-GRID\] mat=(\S+) mip=\d+ map=\d+x(\d+) pitch=([\d.]+)", L.strip())
        if m:
            mats.setdefault(m.group(1), {})["pitch"] = float(m.group(3))
    return mats


def host_breaklines(runs):
    """The column lines V4Bake's MatGrid::colLine builds for one band."""
    out = []
    for lo, hi in runs:
        out += [lo - PAD, hi + PAD]
        if (hi - PAD) - (lo + PAD) >= MINFLOOR:
            out += [lo + PAD, hi - PAD]
        else:
            out += [0.5 * (lo + hi)]
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--mat", default=None)
    ap.add_argument("--near", type=float, default=4.0,
                    help="texels: how close to a host breakline counts as unsnapped")
    a = ap.parse_args()
    for mat, bs in parse(a.log).items():
        if a.mat and mat != a.mat:
            continue
        pitch = bs.pop("pitch", 0.0)
        print("== %s  %d bands, pitch %.1f texels" % (mat, len(bs), pitch))
        if pitch:
            for b, runs in sorted(bs.items()):
                print("   band %d phase(s): %s" %
                      (b, [round((0.5 * (lo + hi)) % pitch, 2) for lo, hi in runs]))
        for h, hruns in sorted(bs.items()):
            host = host_breaklines(hruns)
            rows = []
            for f, fruns in sorted(bs.items()):
                if f == h:
                    continue
                for lo, hi in fruns:
                    c = 0.5 * (lo + hi)
                    d = min(abs(c - x) for x in host)
                    inrec = any(l2 - PAD <= c <= h2 + PAD for l2, h2 in hruns)
                    rows.append((c, d, inrec, f))
            rows.sort()
            near = [r for r in rows if r[1] < a.near]
            print("   host band %d: %d injected, %d within %.1f texels of a host "
                  "breakline, %d INSIDE a host recess"
                  % (h, len(rows), len(near), a.near, sum(1 for r in rows if r[2])))
            for c, d, inrec, f in near:
                print("      x=%8.2f  nearest host breakline %5.2f tex  %s (band %d)"
                      % (c, d, "INSIDE HOST RECESS" if inrec else "block interior", f))


if __name__ == "__main__":
    main()
