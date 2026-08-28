#!/usr/bin/env python3
# nspace_relief.py — NORMAL-space engine-vs-reference height per reference relief
# class (groove / bevel / plateau) per authored plane, from the refdiff raw dumps.
# The refdiff detector's dz is RAY-space; at grazing incidence a normal-space
# offset is stretched 1/cos. This tool reports the normal-space numbers the
# bake is actually responsible for.  usage: nspace_relief.py <rawroot> <arm> [planes...]
import sys, os, glob, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refdiff_detect as rd
root = sys.argv[1]; arm = sys.argv[2]
planes_want = [int(p) for p in sys.argv[3:]] or [43, 45, 48, 49]
bare = os.path.join(root, 'bare_A')
cam, mats, afaces = rd.parse_refplane(glob.glob(os.path.join(bare, '*_refplane.txt'))[0])
hms = {m: rd.load_hm(bare, m, mats[m]) for m in mats}
img, z16, npk, matp, W, H, zscale = rd.load_raw(os.path.join(root, arm))
zeng = (np.float32(0xFF80) - z16.astype(np.float32)) / zscale; zeng[z16 == 0] = np.nan
half, ysign = 0.5, -1.0
nref, zref, mm, dom, ok, pid = rd.build_reference(cam, afaces, mats, hms, W, H, half, ysign, 1.0)
nref0, zref0, mm0, dom0, ok0, pid0 = rd.build_reference(cam, afaces, mats, hms, W, H, half, ysign, 0.0)
src = cam['src']; M = cam['M']
pxs = (np.arange(W, dtype=np.float64)[None, :] + half - cam['cx']) / cam['px']
pys = ysign * (np.arange(H, dtype=np.float64)[:, None] + half - cam['cy']) / cam['py']
Rw = (pxs[..., None] * M[0][None, None, :] + np.broadcast_to(pys[..., None], (H, W, 3)) * M[1][None, None, :] + M[2][None, None, :])
good = ok & ok0 & np.isfinite(zeng) & np.isfinite(zref) & np.isfinite(zref0) & (mm > 0)
for p in planes_want:
    sel = good & (pid == p)
    if sel.sum() < 1000: continue
    ys, xs = np.nonzero(sel)
    P0 = src + zref0[ys, xs, None] * Rw[ys, xs]
    c = P0.mean(0); u, s, vt = np.linalg.svd(P0[::50] - c, full_matrices=False); n = vt[2]
    if n @ (c - src) > 0: n = -n
    d = n @ c
    e = (src + zeng[ys, xs, None] * Rw[ys, xs]) @ n - d
    r = (src + zref[ys, xs, None] * Rw[ys, xs]) @ n - d
    for cls, m in (('groove r<-0.03', r < -0.03), ('bevel -0.03..0', (r >= -0.03) & (r < 0)), ('plateau r>=0', r >= 0)):
        if m.sum() < 200: continue
        de = (e - r)[m]
        print('%s plane %2d %-15s n=%7d  e-r med %+.4f mean %+.4f p10 %+.4f p90 %+.4f | ref med %+.4f eng med %+.4f' %
              (arm, p, cls, m.sum(), np.median(de), de.mean(), np.percentile(de, 10), np.percentile(de, 90), np.median(r[m]), np.median(e[m])))
