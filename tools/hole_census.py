#!/usr/bin/env python3
"""hole census: pixels with ZPage16 == 0 (nothing rasterised) in a snapshot's depth dump.
usage: holes.py <dir> [<dir> ...]   (each dir holds one greets_t*_depth.z16, 1920x1080)"""
import sys, glob, os
import numpy as np
from scipy import ndimage
W, H = 1920, 1080
for d in sys.argv[1:]:
    zs = glob.glob(os.path.join(d, "greets_t*_depth.z16"))
    if not zs:
        print("%-24s (no depth dump)" % os.path.basename(d)); continue
    z = np.fromfile(zs[0], dtype="<u2").reshape(H, W)
    hole = (z == 0)
    lab, n = ndimage.label(hole)
    sz = ndimage.sum(hole, lab, range(1, n + 1)) if n else []
    top = sorted([int(s) for s in sz], reverse=True)[:5]
    print("%-24s holes=%6d comps=%4d top=%s" % (os.path.basename(d), int(hole.sum()), n, top))
