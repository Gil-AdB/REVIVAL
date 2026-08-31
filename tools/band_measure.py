#!/usr/bin/env python3
"""Mean RGB of the greets ceiling band (rows 5:40, cols 600:1300) of a binary PPM.

Usage: python3 tools/band_measure.py /tmp/m5sg/greets_t005965_color.ppm
The band prints as 'R G B' one decimal each; the M5 divergence reads ~85 66 48
where a correct machine reads ~85 134 139 (docs/SHADOW_GUARD.md §6).
"""
import sys
import numpy as np

with open(sys.argv[1], "rb") as f:
    assert f.readline().strip() == b"P6", "not a binary PPM"
    line = f.readline()
    while line.startswith(b"#"):
        line = f.readline()
    w, h = map(int, line.split())
    f.readline()  # maxval
    a = np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3)
print("%.1f %.1f %.1f" % tuple(a[5:40, 600:1300].reshape(-1, 3).mean(axis=0)))
