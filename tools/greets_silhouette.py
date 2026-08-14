#!/usr/bin/env python3
"""SILHOUETTE CRENELLATION metric for the greets displacement arms.

WHY THIS EXISTS
---------------
The whole void/black metric family (void = popcount(z16==0), black =
popcount(max(r,g,b)==0)) is STRUCTURALLY BLIND to the defect this measures.
Silhouette see-through — background visible BETWEEN individual protruding
blocks at a wall's screen edge — is neither a z==0 pixel nor a pure-black
pixel: it is lit background at a plausible depth. Every census in the S1d
campaign could therefore score a perfectly straight wall edge as flawless,
and one did: S1d-5 stage 2 reported "wall-top crenellation PIXEL-IDENTICAL
to the mode-1 reference (see-through preserved)" while comparing shell mode
2 against shell mode 1 — two arms carrying the SAME defect — never against
the tessellation bake, which is what the user's eye compares against.

WHAT IT MEASURES
----------------
The authored wall edge is a straight 3D segment, so it projects to a
STRAIGHT SCREEN LINE. Any deviation of the rendered silhouette from that
line is displacement-induced, and its size is the crenellation. So:

  1. Per row y of a band, walk x across a window and take the first pixel
     whose depth is NEAR (z16 >= --thresh). That is the silhouette x_s(y).
     Depth, not colour: colour separates the wall from a lit background only
     by luck, depth separates them by construction (the near/far split at
     these poses is bimodal with a ~5000-count gap — check with --hist).
  2. Fit a Theil-Sen line to x_s(y) (robust: block protrusions must not
     drag the fit) and report the residual.

  std     RMS deviation from the line, px   <- the headline number
  p95     95th percentile |deviation|, px
  rng     max - min of x_s, px
  area    sum |deviation|, px^2 — the background/foreground area the relief
          exchanges at the silhouette
  tv-net  total variation of x_s MINUS |net displacement|. Zero for ANY
          monotone straight or tilted line; grows only when the edge goes
          out and comes back, i.e. it counts crenellation and ignores tilt.
  off     median x_s relative to the FIRST directory given (pass the flat
          arm first): how far the whole silhouette sits from the authored
          edge. This is the LID OVERHANG when it is negative.

VALIDATION — the three arms whose answer is known by eye, t=5877 cam
15.5497618,3.4823668,-59.5607719,-0.524191499,-0.0974417627,0.846008122,
1920x1080, band y 250..860 / x 880..1120, thresh 62000:

  arm                              med   off    std   p95  rng   area  tv-net
  flat (no displacement)         978.0   0.0   0.46  1.00    1    188       0
  tessellation @0.18             983.0  +5.0   2.43  5.00   17   1045      42
  tessellation @0.30             985.0  +7.0   4.00  8.27   27   1696      66
  shell, the S1d-5b.10 arm       950.0 -28.0   0.35  1.00    1     85       0

Flat and tessellation separate 5.3x on std; the shell arm scores BELOW the
undisplaced wall — it is straighter than flat — while sitting 28 px outside
the authored footprint. That is the defect, and a metric that could not put
those three in that order would not be ready to judge a fix.

USAGE
-----
  FDS_SNAPSHOT_ZDUMP=1 ... ./DEMO --snapshot=greets@t=5877 --out=DIR ...
  tools/greets_silhouette.py DIR_flat DIR_tess DIR_arm [...]

Each DIR must hold one *_depth.z16 (raw ZPage16, uint16 LE, xres*yres).
Larger z16 = NEARER (zEnc = 0xFF80 - g_zscale*z), so "near" is >= thresh.
Defaults are the t=5877 near-wall LEFT silhouette at 1920x1080; --band and
--thresh retarget it. --hist prints the depth histogram of the band so a new
pose's near/far split can be read off rather than guessed.
"""
import argparse, glob, os, sys
import numpy as np


def load_depth(d, w, h):
    fs = glob.glob(os.path.join(d, '*_depth.z16'))
    if not fs:
        sys.exit(f"{d}: no *_depth.z16 (render with FDS_SNAPSHOT_ZDUMP=1)")
    z = np.fromfile(fs[0], dtype='<u2')
    if z.size != w * h:
        sys.exit(f"{d}: depth has {z.size} words, expected {w}x{h}={w*h}"
                 " — pass --res")
    return z.reshape(h, w)


def profile(z, band, thresh, side):
    """x of the first NEAR pixel per row; nan where the row has none."""
    y0, y1, x0, x1 = band
    near = z >= thresh
    out = np.full(y1 - y0, np.nan)
    for k, y in enumerate(range(y0, y1)):
        idx = np.nonzero(near[y, x0:x1])[0]
        if idx.size:
            out[k] = x0 + (idx[0] if side == 'left' else idx[-1])
    return out


def theil_sen(y, x, cap=200):
    """Median-of-pairwise-slopes fit. Robust to the protrusions we measure —
    a least-squares fit would be dragged by them and understate the residual."""
    step = max(1, len(y) // cap)
    ys, xs = y[::step], x[::step]
    sl = [(xs[j] - xs[i]) / (ys[j] - ys[i])
          for i in range(len(ys)) for j in range(i + 1, len(ys))]
    m = np.median(sl) if sl else 0.0
    return m, np.median(x - m * y)


def stats(p):
    ok = ~np.isnan(p)
    y = np.arange(len(p), dtype=float)[ok]
    x = p[ok]
    if x.size < 8:
        return None
    m, b = theil_sen(y, x)
    r = x - (m * y + b)
    return dict(n=int(x.size), med=float(np.median(x)), std=float(r.std()),
                p95=float(np.percentile(np.abs(r), 95)),
                rng=float(x.max() - x.min()), area=float(np.abs(r).sum()),
                tvx=float(np.abs(np.diff(x)).sum() - abs(x[-1] - x[0])))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dirs', nargs='+', help='snapshot dirs; the FIRST is the '
                    'reference the `off` column is measured against')
    ap.add_argument('--res', default='1920x1080')
    ap.add_argument('--band', default='250,860,880,1120',
                    help='y0,y1,x0,x1 of the search window (default: the '
                         't=5877 near-wall left silhouette at 1080p)')
    ap.add_argument('--thresh', type=int, default=62000,
                    help='z16 at or above which a pixel is the NEAR surface')
    ap.add_argument('--side', choices=('left', 'right'), default='left',
                    help='which edge of the near surface to trace')
    ap.add_argument('--hist', action='store_true',
                    help='print the band depth histogram and exit (use it to '
                         'pick --thresh at a new pose)')
    a = ap.parse_args()
    w, h = (int(v) for v in a.res.split('x'))
    band = tuple(int(v) for v in a.band.split(','))

    if a.hist:
        for d in a.dirs:
            z = load_depth(d, w, h)[band[0]:band[1], band[2]:band[3]]
            cnt, edge = np.histogram(z, bins=20)
            print(d)
            for i in range(20):
                print(f"   {int(edge[i]):6d}-{int(edge[i+1]):6d}: {cnt[i]}")
        return

    ref = None
    print(f"{'arm':28s} {'n':>4s} {'med':>7s} {'off':>6s} {'std':>6s} "
          f"{'p95':>6s} {'rng':>5s} {'area':>8s} {'tv-net':>7s}")
    for d in a.dirs:
        s = stats(profile(load_depth(d, w, h), band, a.thresh, a.side))
        if s is None:
            print(f"{os.path.basename(d.rstrip('/')):28s}  (no silhouette in band)")
            continue
        if ref is None:
            ref = s['med']
        print(f"{os.path.basename(d.rstrip('/')):28s} {s['n']:4d} {s['med']:7.1f} "
              f"{s['med']-ref:6.1f} {s['std']:6.2f} {s['p95']:6.2f} "
              f"{s['rng']:5.0f} {s['area']:8.0f} {s['tvx']:7.0f}")


if __name__ == '__main__':
    main()
