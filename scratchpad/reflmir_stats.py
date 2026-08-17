#!/usr/bin/env python3
"""reflmir_stats.py DIR_BEFORE DIR_AFTER — per-pose changed-pixel table."""
import sys, os, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reflmir_img import read_ppm  # noqa

b, a = sys.argv[1], sys.argv[2]
print("%-22s %10s %8s %7s %9s   %s" % ("pose", "changed px", "%", "max|d|", "mean|d|", "changed bbox"))
print("-" * 92)
for f in sorted(glob.glob(os.path.join(b, "*_color.ppm"))):
    n = os.path.basename(f)
    g = os.path.join(a, n)
    if not os.path.exists(g):
        continue
    A = read_ppm(f).astype(np.int16)
    B = read_ppm(g).astype(np.int16)
    D = np.abs(A - B).max(axis=2)
    ch = D > 0
    tot = D.shape[0] * D.shape[1]
    if not ch.any():
        print("%-22s %10d %8s %7s %9s   %s" % (n[:-10], 0, "0.000", "-", "-", "BYTE-IDENTICAL"))
        continue
    ys, xs = np.where(ch)
    print("%-22s %10d %7.3f%% %7d %9.2f   x=[%d..%d] y=[%d..%d]"
          % (n[:-10], ch.sum(), 100.0 * ch.sum() / tot, D.max(), D[ch].mean(),
             xs.min(), xs.max(), ys.min(), ys.max()))
