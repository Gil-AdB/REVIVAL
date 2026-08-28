#!/usr/bin/env python3
# bridging_verify.py — direct test of the bridging-face premise (handoff round).
# For every protruding pixel inside Gil-Ad's two marked regions (r3 arm, cam A):
# find the RENDERED bake face containing that pixel's world point (from
# displace_faces.txt) and classify it against the authored wall planes:
#   BRIDGE  = corners assign to different authored planes (face spans the junction)
#   SAME    = all corners on one plane (protrusion is that wall's own surface)
#   NOFACE  = no bake face within tolerance (rendered surface not in the dump)
# The premise ("juts are faces whose corners are individually legal") predicts
# BRIDGE-dominant with small corner off-plane excess.
import sys, os, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refdiff_detect as rd

root = sys.argv[1] if len(sys.argv) > 1 else 'docs/img/refdiff/raw'
facef = sys.argv[2] if len(sys.argv) > 2 else 'Runtime/displace_faces.txt'
rawdir = os.path.join(root, 'r3_A'); baredir = os.path.join(root, 'bare_A')

img, z16, npk, matp, W, H, zscale = rd.load_raw(rawdir)
cam, mats, afaces = rd.parse_refplane(glob.glob(os.path.join(baredir, '*_refplane.txt'))[0])
hms = {m: rd.load_hm(baredir, m, mats[m]) for m in mats}
zeng = (np.float32(0xFF80) - z16.astype(np.float32)) / zscale
zeng[z16 == 0] = np.nan
best = None
for half in (0.5, 0.0):
    for ysign in (1.0, -1.0):
        nref, zref, mm, dom, ok, pid = rd.build_reference(cam, afaces, mats, hms, W, H, half, ysign, 1.0)
        cand = ok & np.isfinite(zeng) & np.isfinite(zref) & (mm > 0)
        res = float(np.nanmedian(np.abs(zref[cand] - zeng[cand]))) if cand.sum() > 1000 else np.inf
        if best is None or res < best[0]: best = (res, half, ysign, zref, ok)
res, half, ysign, zref, ok = best
print('camera pick half=%.1f ysign=%+.0f med|dz|=%.4f' % (half, ysign, res))
dz = zref - zeng
src = cam['src']; M = cam['M']
pxs = (np.arange(W, dtype=np.float64)[None, :] + half - cam['cx']) / cam['px']
pys = ysign * (np.arange(H, dtype=np.float64)[:, None] + half - cam['cy']) / cam['py']
Rw = (pxs[..., None] * M[0][None, None, :] +
      np.broadcast_to(pys[..., None], (H, W, 3)) * M[1][None, None, :] + M[2][None, None, :])

# authored plane set (dedup by normal+offset)
planes = []
for (mesh, mat, A, B, C, uv, N) in afaces:
    n = np.cross(B - A, C - A); nl = np.linalg.norm(n)
    if nl < 1e-9: continue
    n = n / nl
    if n @ (A - src) > 0: n = -n
    d = n @ A
    for i, (pn, pd, pmat) in enumerate(planes):
        if abs(pn @ n) > 0.999 and abs((pn @ n) * d - pd) < 0.05: break
    else:
        planes.append((n, d, mat))
print('%d authored planes' % len(planes))

# bake faces
fmat = []; fverts = []
for line in open(facef):
    if line.startswith('#'): continue
    t = line.split()
    if not t[0].startswith('rooms'): continue
    fmat.append(t[0]); fverts.append(list(map(float, t[1:10])))
FV = np.array(fverts).reshape(-1, 3, 3)
print('%d bake faces (rooms*)' % len(FV))
lo = FV.min(1) - 0.06; hi = FV.max(1) + 0.06

def pt_tri_dist(p, a, b, c):
    ab = b - a; ac = c - a; ap = p - a
    d1 = ab @ ap; d2 = ac @ ap
    if d1 <= 0 and d2 <= 0: return np.linalg.norm(ap)
    bp = p - b; d3 = ab @ bp; d4 = ac @ bp
    if d3 >= 0 and d4 <= d3: return np.linalg.norm(bp)
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        return np.linalg.norm(ap - ab * (d1 / (ab @ ab)))
    cp = p - c; d5 = ab @ cp; d6 = ac @ cp
    if d6 >= 0 and d5 <= d6: return np.linalg.norm(cp)
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        return np.linalg.norm(ap - ac * (d2 / (ac @ ac)))
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        bc = c - b
        return np.linalg.norm(bp - bc * ((bc @ bp) / (bc @ bc)))
    n = np.cross(ab, ac); n = n / np.linalg.norm(n)
    return abs(ap @ n)

yy, xx = np.mgrid[0:H, 0:W]
rect = (xx >= 1080) & (xx <= 1235) & (yy >= 315) & (yy <= 415)
ell = (((xx - 1160) / 130.0) ** 2 + ((yy - 650) / 115.0) ** 2) <= 1.0
rng = np.random.default_rng(7)
for rname, rmask in (('MARK-rect', rect), ('MARK-ellipse', ell)):
    m = rmask & ok & np.isfinite(dz) & (dz > 0.08)
    ys, xs = np.nonzero(m)
    if len(ys) == 0: print(rname, ': no protruding pixels'); continue
    sel = rng.choice(len(ys), size=min(400, len(ys)), replace=False)
    cls = {'BRIDGE': 0, 'SAME': 0, 'NOFACE': 0}
    excesses = []; span_pairs = {}
    for k in sel:
        y, x = ys[k], xs[k]
        P = src + zeng[y, x] * Rw[y, x]
        cand = np.nonzero(np.all((P >= lo) & (P <= hi), axis=1))[0]
        bestd, bi = 1e9, -1
        for i in cand:
            d = pt_tri_dist(P, FV[i, 0], FV[i, 1], FV[i, 2])
            if d < bestd: bestd, bi = d, i
        if bi < 0 or bestd > 0.05: cls['NOFACE'] += 1; continue
        # classify corners by nearest authored plane
        pids = []
        for c in range(3):
            dists = [abs(pn @ FV[bi, c] - pd) for (pn, pd, pmat) in planes]
            pids.append(int(np.argmin(dists)))
        if len(set(pids)) > 1:
            cls['BRIDGE'] += 1
            key = tuple(sorted(set(pids))); span_pairs[key] = span_pairs.get(key, 0) + 1
        else:
            cls['SAME'] += 1
            key = (pids[0],); span_pairs[key] = span_pairs.get(key, 0) + 1
        # corner legality: distance of each corner ALONG its own plane normal
        for c in range(3):
            pn, pd, pmat = planes[pids[c]]
            excesses.append(pn @ FV[bi, c] - pd)
    n = sum(cls.values())
    print('%s: n=%d  BRIDGE %.1f%%  SAME %.1f%%  NOFACE %.1f%%' %
          (rname, n, 100.0 * cls['BRIDGE'] / n, 100.0 * cls['SAME'] / n,
           100.0 * cls['NOFACE'] / n))
    if excesses:
        e = np.array(excesses)
        print('  corner height above own plane: med %+.4f p90 %+.4f max %+.4f' %
              (np.median(e), np.percentile(e, 90), e.max()))
    top = sorted(span_pairs.items(), key=lambda kv: -kv[1])[:4]
    for key, cnt in top:
        desc = ' + '.join('P%d(n=%.2f,%.2f,%.2f)' % ((i,) + tuple(planes[i][0])) for i in key)
        print('  %4d px on faces of %s' % (cnt, desc))
