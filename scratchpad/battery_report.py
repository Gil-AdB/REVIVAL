#!/usr/bin/env python3
"""Constraint-battery report: per pose, ON-vs-OFF diff census + background
(near-black, the z==0 proxy) count per arm."""
import glob, os
import numpy as np
from PIL import Image

root = "/tmp/xsec/battery"
poses = sorted({os.path.basename(d).rsplit("_", 1)[0] for d in glob.glob(root + "/t*_on")})
print(f"{'pose':8} {'diffpx':>8} {'max':>4} {'bbox':>24} {'bg_off':>7} {'bg_on':>7} {'d_bg':>5}")
for p in poses:
    try:
        a = np.asarray(Image.open(f"{root}/{p}_off/greets_t{int(p[1:]):06d}_color.ppm").convert("RGB"), np.int16)
        b = np.asarray(Image.open(f"{root}/{p}_on/greets_t{int(p[1:]):06d}_color.ppm").convert("RGB"), np.int16)
    except Exception as e:
        print(p, "MISSING", e)
        continue
    d = np.abs(a - b).max(axis=2)
    ys, xs = np.nonzero(d > 0)
    bbox = f"x{xs.min()}-{xs.max()} y{ys.min()}-{ys.max()}" if len(xs) else "-"
    bg_a = int((a.max(axis=2) < 4).sum())
    bg_b = int((b.max(axis=2) < 4).sum())
    print(f"{p:8} {int((d>0).sum()):>8} {int(d.max()):>4} {bbox:>24} {bg_a:>7} {bg_b:>7} {bg_b-bg_a:>+5}")
