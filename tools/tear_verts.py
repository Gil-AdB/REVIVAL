#!/usr/bin/env python3
# tear_verts.py <rawroot> <tag> <bare_tag> <displace_dump.txt> [outdir]
# For every HOLE cluster (connected component of tear pixels) at a pose:
# world location on the reference surface, the reference planes involved,
# class (base/seam/panel), and the bake verts within 0.15 u of the cluster's
# reference points — per material: count, median |dv|, median motion along
# the plane normal, created/weld flags. This is the join from "a slit the
# eye sees" to "the two bakes' verts on either side of it".
import sys, os, glob
import numpy as np
from scipy import ndimage as ndi
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tear_detect as td

root, tag, bare, dumpf = sys.argv[1:5]
outdir = sys.argv[5] if len(sys.argv) > 5 else 'docs/img/tears'
n = td.detect(os.path.join(root, tag), os.path.join(root, bare), tag, outdir, os.devnull)
L = td.detect.last
cam = L['cam']; W, H = L['W'], L['H']; half, ysign = L['half'], L['ysign']
M = cam['M']; src = cam['src']
pxs = (np.arange(W, dtype=np.float64)[None, :] + half - cam['cx']) / cam['px']
pys = ysign * (np.arange(H, dtype=np.float64)[:, None] + half - cam['cy']) / cam['py']
Rw = (pxs[..., None] * M[0][None, None, :] + np.broadcast_to(pys[..., None], (H, W, 3)) * M[1][None, None, :] + M[2][None, None, :])
P = src[None, None, :] + np.nan_to_num(L['zref'])[..., None] * Rw
# verts
V = []; MAT = []; DV = []; CR = []; WD = []
for line in open(dumpf):
    if line.startswith('#'): continue
    t = line.split()
    if len(t) < 15: continue
    V.append(list(map(float, t[0:3]))); DV.append(list(map(float, t[3:6]))); MAT.append(t[10]); CR.append(int(t[13])); WD.append(int(t[14]))
V = np.array(V); DV = np.array(DV); MAT = np.array(MAT); CR = np.array(CR); WD = np.array(WD)
from scipy.spatial import cKDTree
tree = cKDTree(V)
tear = L['tear']
lab, ncl = ndi.label(ndi.binary_dilation(tear, iterations=1))
rows = []
for c in range(1, ncl + 1):
    m = (lab == c) & tear
    npx = int(m.sum())
    if npx < 6: continue
    pts = P[m]
    ctr = pts.mean(axis=0); ext = pts.max(axis=0) - pts.min(axis=0)
    cls = 'base' if L['base'][m].any() else ('seam' if L['seam'][m].any() else 'panel')
    planes = sorted(set(int(x) for x in L['pid'][m] if x >= 0))
    # sample up to 40 points, gather verts within 0.15
    idx = np.linspace(0, len(pts) - 1, min(40, len(pts))).astype(int)
    near = set()
    for i in idx:
        for j in tree.query_ball_point(pts[i], 0.15): near.add(j)
    near = np.array(sorted(near), dtype=int)
    permat = {}
    for j in near:
        permat.setdefault(MAT[j], []).append(j)
    desc = []
    for mat, js in sorted(permat.items()):
        js = np.array(js)
        mag = np.linalg.norm(DV[js], axis=1)
        desc.append('%s n=%d |dv| med %.3f max %.3f created %d weld %d' % (mat, len(js), np.median(mag), mag.max(), int(CR[js].sum()), int(WD[js].sum())))
    yy, xx = np.nonzero(m)
    rows.append((npx, cls, planes, ctr, ext, (int(xx.min()), int(yy.min()), int(xx.max()), int(yy.max())), desc))
rows.sort(key=lambda r: -r[0])
out = open(os.path.join(outdir, 'tear_verts_%s.txt' % tag), 'w')
for npx, cls, planes, ctr, ext, bbox, desc in rows:
    line = '%5d px %-5s planes %-10s ctr (%.2f %.2f %.2f) ext (%.2f %.2f %.2f) px-bbox %s\n      %s' % (
        npx, cls, ','.join(map(str, planes)), *ctr, *ext, bbox, '\n      '.join(desc) if desc else 'NO VERTS within 0.15u')
    print(line); out.write(line + '\n')
