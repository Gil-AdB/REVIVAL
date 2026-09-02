#!/usr/bin/env python3
"""Per-pixel UV step across every stone seam on screen, from a snapshot's G-buffer dump.

Answers "is the UV continuous across this seam?" numerically, for every seam
in view at once, with no reliance on the bake: the G-buffer txtr word carries
the texel column/row the rasteriser sampled (mip:4 | matID:8 | swizzled UV:20,
SimdHelpers.h), which decodes to the UV the surface carries at that pixel,
wrapped to one tile - the same decode --uv_viz paints (LightmapBake.cpp
UvViz_DecodeTexel). A world-adjacent pixel pair that straddles a face boundary
with a wrapped step far above the in-face pixel gradient is a UV discontinuity.

Needs, in <dir>, from one snapshot run with
  FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 ... --face_id_dump --snapshot=greets@t=<t> --out=<dir>
  greets_t<t>_mat.u32   the txtr plane
  greets_t<t>_face.u32  the face-owner plane
  greets_t<t>_depth.z16 the depth plane (silhouette filter)
  log.txt               stderr of that run: the [GBUFDUMP] id->name table and the [FACEID] face table

Assumes the stone materials share one square power-of-two diffuse map (--tex,
default 1024); verify the decode once against --mat_probe's on-screen readout.

Example (H6194, 2026-09-02):
  python3 tools/uv_seam_census.py <dir> --t 6194
  -> same-material seams (coplanar AND crease) du p90 <= 3 texels; every
     rooms|rooms::mirUV boundary pair 405-443 texels (0.40-0.43 tile).
"""
import argparse, collections, re, sys
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--t", type=int, default=6194)
ap.add_argument("--xres", type=int, default=1920)
ap.add_argument("--yres", type=int, default=1080)
ap.add_argument("--tex", type=int, default=1024, help="stone diffuse map size at mip 0 (square, pow2)")
ap.add_argument("--mats", default="rooms,rooms::mirUV", help="material names counted as stone")
ap.add_argument("--dz", type=float, default=0.05, help="max depth gap (world u) for a pair to count as world-adjacent")
ap.add_argument("--zscale", type=float, default=395.636353)
ap.add_argument("--big", type=float, default=32, help="texel step that counts as a discontinuity")
ap.add_argument("--min-n", type=int, default=10)
a = ap.parse_args()

W, H = a.xres, a.yres
p = f"{a.dir}/greets_t{a.t:06d}_"
mat = np.fromfile(p + "mat.u32", dtype=np.uint32).reshape(H, W)
face = np.fromfile(p + "face.u32", dtype=np.uint32).reshape(H, W) >> 4
z16 = np.fromfile(p + "depth.z16", dtype=np.uint16).reshape(H, W).astype(np.float64)
z = np.where(z16 > 0, (0xFF80 - z16) / a.zscale, np.nan)
log = open(f"{a.dir}/log.txt").read()
names = {int(i): n for i, n in re.findall(r"\[GBUFDUMP\] id=(\d+) name=(.*)", log)}
stone = set(a.mats.split(","))
stone_ids = [i for i, n in names.items() if n in stone]
if not stone_ids:
    sys.exit("no stone material ids in log.txt ([GBUFDUMP] lines missing? run with FDS_SNAPSHOT_GBUFDUMP=1)")
F = {}
pat = (r"\[FACEID\] key=(\d+) mesh=(\S+) fi=(\d+) mat=(\S+) N=\(([-\d.]+),([-\d.]+),([-\d.]+)\) d=([-\d.]+) "
       r"A=\(([-\d.,]+)\) B=\(([-\d.,]+)\) C=\(([-\d.,]+)\) uv=\(([-\d.]+),([-\d.]+)\)\(([-\d.]+),([-\d.]+)\)\(([-\d.]+),([-\d.]+)\)")
for m in re.finditer(pat, log):
    g = m.groups()
    F[int(g[0])] = dict(mesh=g[1], fi=int(g[2]), mat=g[3], N=np.array(list(map(float, g[4:7]))),
                        A=np.array(list(map(float, g[8].split(",")))),
                        uv=np.array(list(map(float, g[11:17]))).reshape(3, 2))
if not F:
    sys.exit("no [FACEID] table in log.txt (run with --face_id_dump)")

# decode the txtr word exactly as UvViz_DecodeTexel
mip = (mat >> 28).astype(np.int32)
mid = ((mat >> 20) & 0xFF).astype(np.int32)
addr = (mat & 0xFFFFF).astype(np.int64)
w = np.maximum(1, a.tex >> mip)
vbits = int(np.log2(a.tex)) - mip
v = (addr >> 2) & (w - 1)
u = ((addr & 3) | ((addr >> (2 + vbits)) << 2)) & (w - 1)
valid = (mat != 0xFFFFFFFF) & (mat != 0xFFFFFFFE) & np.isin(mid, stone_ids) & (z16 > 0)
fu = (u + 0.5) / w
fv = (v + 0.5) / w
print(f"stone ids {{{', '.join(f'{i}:{names[i]}' for i in stone_ids)}}}  faces in table {len(F)}  "
      f"probe ({W//2},{H//2}): {names.get(mid[H//2, W//2], '?')} u,v {u[H//2, W//2]},{v[H//2, W//2]} mip {mip[H//2, W//2]}")


def wrapd(x, y):
    d = np.abs(x - y)
    return np.minimum(d, 1 - d)


def pairs(axis):
    if axis == 0:
        s0, s1 = (slice(None), slice(0, -1)), (slice(None), slice(1, None))
    else:
        s0, s1 = (slice(0, -1), slice(None)), (slice(1, None), slice(None))
    fa, fb = face[s0], face[s1]
    ok = valid[s0] & valid[s1] & (fa != fb) & (fa != 0) & (fb != 0) & (np.abs(z[s0] - z[s1]) < a.dz)
    return fa[ok], fb[ok], wrapd(fu[s0][ok], fu[s1][ok]), wrapd(fv[s0][ok], fv[s1][ok]), mid[s0][ok], mid[s1][ok]


fa, fb, du, dv, ma, mb = [np.concatenate(x) for x in zip(pairs(0), pairs(1))]
known = np.array([(int(x) in F) and (int(y) in F) for x, y in zip(fa, fb)])
fa, fb, du, dv, ma, mb = fa[known], fb[known], du[known], dv[known], ma[known], mb[known]
angs = np.array([np.degrees(np.arccos(np.clip(np.dot(F[int(x)]["N"], F[int(y)]["N"]), -1, 1))) for x, y in zip(fa, fb)])
same = ma == mb
T = a.tex
print(f"world-adjacent stone seam pixel pairs {len(fa)}: same material {same.sum()}, material boundary {(~same).sum()}")
print(f"{'dihedral':15s} {'materials':9s} {'n':>6s}  du texels p50/p90/max      dv p50/p90/max    frac du>{a.big:g}")
for name, sel in [("coplanar <5", angs < 5), ("5-30", (angs >= 5) & (angs < 30)), ("crease >=30", angs >= 30)]:
    for mname, msel in [("same", same), ("boundary", ~same)]:
        s = sel & msel
        if not s.sum():
            continue
        tu, tv = du[s] * T, dv[s] * T
        print(f"{name:15s} {mname:9s} {s.sum():6d}  {np.median(tu):6.1f}/{np.percentile(tu, 90):6.1f}/{tu.max():6.1f}   "
              f"{np.median(tv):5.1f}/{np.percentile(tv, 90):5.1f}/{tv.max():5.1f}    {np.mean(tu > a.big):.3f}")
h0, h1 = (slice(None), slice(0, -1)), (slice(None), slice(1, None))
inface = valid[h0] & valid[h1] & (face[h0] == face[h1]) & (face[h0] != 0)
g = wrapd(fu[h0][inface], fu[h1][inface]) * T
print(f"in-face horizontal pixel gradient, texels: p50 {np.median(g):.2f} p99 {np.percentile(g, 99):.2f}")

agg = collections.defaultdict(list)
for x, y, s, t, d in zip(fa, fb, du, dv, angs):
    k = (int(min(x, y)), int(max(x, y)))
    agg[k].append((s, t, d))
rows = []
for (x, y), L in agg.items():
    L = np.array(L)
    rows.append((x, y, len(L), np.median(L[:, 0]) * T, np.median(L[:, 1]) * T, L[0, 2]))
big = sorted([r for r in rows if r[2] >= a.min_n and r[3] > a.big], key=lambda r: -r[3])
print(f"\nface pairs with a median u step > {a.big:g} texels (n >= {a.min_n}): {len(big)}")
for x, y, n, s, t, d in big:
    A, B = F[x], F[y]
    print(f"  n={n:4d} du={s:6.1f} dv={t:5.1f} dih={d:5.1f}  {A['mat']:13s} fi{A['fi']:<4d} "
          f"A=({A['A'][0]:6.3f},{A['A'][1]:6.3f},{A['A'][2]:8.3f}) u0={A['uv'][0][0]:7.3f}  x  "
          f"{B['mat']:13s} fi{B['fi']:<4d} A=({B['A'][0]:6.3f},{B['A'][1]:6.3f},{B['A'][2]:8.3f}) u0={B['uv'][0][0]:7.3f}")
