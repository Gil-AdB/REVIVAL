#!/usr/bin/env python3
# bulge_detect.py — calibrated bulge detector for greets walls (2026-08-28).
#
# Reads a greets snapshot dump set (color.ppm + norm.u32 [--bulge_dump] +
# mat.u32 [FDS_SNAPSHOT_GBUFDUMP] + depth.z16 [FDS_SNAPSHOT_ZDUMP]) and
# produces, per named region:
#   1. an ARROW OVERLAY png: sparse grid of projected shading normals over the
#      render, colored by the cell's fitted normal-gradient (deg per 100 px)
#   2. a SCANLINE plot png: normal components + depth along one row
#   3. metrics: GBI (median per-cell intra-block normal drift, deg/100px),
#      SWEEP (mean-normal rotation left-third vs right-third and top vs
#      bottom), GEOMBOW (low-pass residual of a perspective-plane fit to the
#      depth, world units) — geometry-vs-shading attribution in one number
#      pair: GEOMBOW says the positions bow; GBI/SWEEP with flat GEOMBOW says
#      the *shading normals* bow.
#
# A flat dressed-stone wall reads: GBI ~ bevel-noise floor, SWEEP ~ 0 within a
# planar panel, GEOMBOW ~ block relief amplitude. A bulge reads: GBI and/or
# SWEEP well above the synthetic-flat floor with either GEOMBOW high (geometry
# -borne) or low (shading-borne).
import sys, struct, math
import numpy as np
from PIL import Image, ImageDraw

WALL_IDS = {10, 40}  # rooms + rooms::mirUV — the walls under judgment ONLY

def load_ppm(p):
    with open(p, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        return np.frombuffer(f.read(w*h*3), np.uint8).reshape(h, w, 3), w, h

def oct_decode(packed):
    qx = (packed & 0xffff).astype(np.int16).astype(np.float32) / 32767.0
    qy = ((packed >> 16) & 0xffff).astype(np.int16).astype(np.float32) / 32767.0
    az = 1.0 - np.abs(qx) - np.abs(qy)
    fold = az < 0
    fx = (1.0 - np.abs(qy)) * np.sign(np.where(qx >= 0, 1.0, -1.0))
    fy = (1.0 - np.abs(qx)) * np.sign(np.where(qy >= 0, 1.0, -1.0))
    ox = np.where(fold, fx, qx); oy = np.where(fold, fy, qy)
    n = np.stack([ox, oy, az], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)

def mat_ids(mat_u32):
    # packed mip:4 | matID:8 | swizzledUV:20  (sentinels 0xFFFFFFFF/FE pass)
    return ((mat_u32 >> 20) & 0xFF).astype(np.int32)

def cell_gradient_deg100(n, mask, zv=None, cell=64):
    """Per-cell linear fit of the normal field; |gradient| in deg per 100px."""
    h, w, _ = n.shape
    out = np.full(((h+cell-1)//cell, (w+cell-1)//cell), np.nan)
    for cy in range(out.shape[0]):
        for cx in range(out.shape[1]):
            ys, xs = cy*cell, cx*cell
            m = mask[ys:ys+cell, xs:xs+cell].copy()
            if zv is not None and m.any():
                zc = zv[ys:ys+cell, xs:xs+cell]
                med = np.nanmedian(zc[m])
                m &= np.abs(zc - med) < 0.75          # depth-coherent: one surface per cell
            if m.sum() < cell*cell*0.35: continue
            sub = n[ys:ys+cell, xs:xs+cell][m]
            yy, xx = np.mgrid[0:m.shape[0], 0:m.shape[1]]
            A = np.stack([xx[m], yy[m], np.ones(m.sum())], -1).astype(np.float64)
            g, *_ = np.linalg.lstsq(A, sub.astype(np.float64), rcond=None)
            # rotation rate = |d n/d px| of the fitted plane, worst direction
            rate = max(np.linalg.norm(g[0]), np.linalg.norm(g[1]))
            out[cy, cx] = math.degrees(rate) * 100.0
    return out

def mean_dir(n, mask):
    if mask.sum() == 0: return None
    v = n[mask].mean(0); return v / np.linalg.norm(v)

def ang(a, b):
    if a is None or b is None: return float('nan')
    return math.degrees(math.acos(np.clip(np.dot(a, b), -1, 1)))

def analyze(rawdir, tag, regions, outdir, zscale, scan_y=None):
    import os
    ppm = None
    for f in os.listdir(rawdir):
        if f.endswith('_color.ppm'): ppm = os.path.join(rawdir, f)
    base = ppm[:-len('_color.ppm')]
    img, w, h = load_ppm(ppm)
    norm = oct_decode(np.fromfile(base + '_norm.u32', np.uint32).reshape(h, w))
    mid = mat_ids(np.fromfile(base + '_mat.u32', np.uint32).reshape(h, w))
    z16 = np.fromfile(base + '_depth.z16', np.uint16).reshape(h, w).astype(np.float64)
    zview = np.where(z16 > 0, (0xFF80 - z16) / zscale, np.nan)
    wall = np.isin(mid, list(WALL_IDS)) & (z16 > 0)

    ov = Image.fromarray(img).convert('RGB')
    d = ImageDraw.Draw(ov)
    rows = []
    for name, (x0, y0, x1, y1) in regions.items():
        rm = np.zeros((h, w), bool); rm[y0:y1, x0:x1] = True
        m = rm & wall
        if m.any():
            # keep the dominant depth-continuous surface: histogram depth, keep
            # pixels within 1.5u of the modal 0.5u-bin (kills see-past-arris px)
            zz = zview[m]
            hist, edges = np.histogram(zz[np.isfinite(zz)], bins=np.arange(0, np.nanmax(zz)+0.5, 0.5))
            mode = edges[np.argmax(hist)] + 0.25
            keep = np.zeros((h, w), bool)
            keep[m] = np.abs(zview[m] - mode) < 1.5
            if keep.sum() > 0.25 * m.sum(): m = keep
        sub_n, sub_m = norm[y0:y1, x0:x1], m[y0:y1, x0:x1]
        cg = cell_gradient_deg100(sub_n, sub_m, zview[y0:y1, x0:x1])
        gbi = float(np.nanmedian(cg))
        thirds_x = (x1 - x0)//3; thirds_y = (y1 - y0)//3
        L = m.copy(); L[:, x0+thirds_x:] = False
        R = m.copy(); R[:, :x1-thirds_x] = False
        T = m.copy(); T[y0+thirds_y:, :] = False
        B = m.copy(); B[:y1-thirds_y, :] = False
        sweep_h = ang(mean_dir(norm, L), mean_dir(norm, R))
        sweep_v = ang(mean_dir(norm, T), mean_dir(norm, B))
        # geometry twin: perspective plane fit -> 1/z affine in (x,y)
        yy, xx = np.nonzero(m)
        invz = 1.0 / zview[m]
        A = np.stack([xx, yy, np.ones(len(xx))], -1)
        c, *_ = np.linalg.lstsq(A, invz, rcond=None)
        pred = A @ c
        resid_z = (1.0/invz) - (1.0/np.maximum(pred, 1e-9))
        # low-pass the residual on the grid to see the bow, not the blocks
        grid = np.full((h, w), np.nan); grid[m] = resid_z
        sub = grid[y0:y1, x0:x1]
        k = 31
        pad = np.nan_to_num(sub); cnt = (~np.isnan(sub)).astype(float)
        ker = np.ones((k, k))
        from numpy.lib.stride_tricks import sliding_window_view as swv
        def boxblur(a):
            ap = np.pad(a, k//2)
            return swv(ap, (k, k)).reshape(a.shape[0], a.shape[1], -1).sum(-1)
        low = boxblur(pad) / np.maximum(boxblur(cnt), 1)
        bow = float(np.nanmax(np.abs(np.where(cnt > 0, low, np.nan))))
        rows.append((tag, name, gbi, sweep_h, sweep_v, bow, int(m.sum())))
        # arrows over this region
        step = 28
        for ay in range(y0+step//2, y1, step):
            for ax_ in range(x0+step//2, x1, step):
                if not m[ay, ax_]: continue
                cyx = cg[(ay-y0)//64, (ax_-x0)//64]
                col = (60, 220, 60)
                if not np.isnan(cyx):
                    if cyx > 15: col = (255, 40, 40)
                    elif cyx > 8: col = (255, 140, 0)
                    elif cyx > 3: col = (250, 240, 60)
                nx, ny = norm[ay, ax_, 0], norm[ay, ax_, 1]
                L2 = 13.0
                d.line([(ax_, ay), (ax_ + nx*L2, ay - ny*L2)], fill=col, width=2)
                d.ellipse([ax_-1.5, ay-1.5, ax_+1.5, ay+1.5], fill=col)
        d.rectangle([x0, y0, x1, y1], outline=(255, 0, 255))
        d.text((x0+4, y0+4), name, fill=(255, 0, 255))
    ov.save(os.path.join(outdir, f'overlay_{tag}.png'))
    # scanline plot: one row through the widest region
    if scan_y is not None:
        name, (x0, y0, x1, y1) = list(regions.items())[0]
        for rn, (rx0, ry0, rx1, ry1) in regions.items():
            if ry0 <= scan_y < ry1: name, (x0, y0, x1, y1) = rn, (rx0, ry0, rx1, ry1)
        W, H = x1-x0, 460
        pl = Image.new('RGB', (W, H+80), (18, 18, 22)); dp = ImageDraw.Draw(pl)
        strip = Image.fromarray(img[scan_y-30:scan_y+30, x0:x1]).convert('RGB')
        pl.paste(strip, (0, 0))
        mrow = wall[scan_y, x0:x1]
        colors = [(255, 90, 90), (110, 255, 110), (110, 160, 255)]
        for ci in range(3):
            comp = norm[scan_y, x0:x1, ci]
            pts = [(x, 70 + int((1 - comp[x]) * 0.5 * (H-90))) for x in range(W) if mrow[x]]
            for i in range(1, len(pts)):
                if pts[i][0] - pts[i-1][0] <= 2: dp.line([pts[i-1], pts[i]], fill=colors[ci])
        zrow = zview[scan_y, x0:x1]
        zn = (zrow - np.nanmin(zrow)) / max(1e-9, (np.nanmax(zrow) - np.nanmin(zrow)))
        pts = [(x, 70 + int((1 - zn[x]) * 0.5 * (H-90))) for x in range(W) if mrow[x] and np.isfinite(zn[x])]
        for i in range(1, len(pts)):
            if pts[i][0] - pts[i-1][0] <= 2: dp.line([pts[i-1], pts[i]], fill=(255, 255, 255))
        dp.text((6, H+58), f'y={scan_y} {name}: nx=red ny=green nz=blue depth=white (norm. scale)', fill=(230, 230, 230))
        pl.save(os.path.join(outdir, f'scanline_{tag}_{name}_y{scan_y}.png'))
    return rows

def synthetic_floor():
    """Numeric quiet floor: flat blocks + bevel bands, nmap-like noise."""
    h, w = 512, 768
    n = np.zeros((h, w, 3), np.float32); n[..., 2] = 1.0
    for bx in range(0, w, 128):   # vertical grout bevel bands, ±25 deg
        a = math.radians(25)
        n[:, bx:bx+6, 0] = math.sin(a); n[:, bx:bx+6, 2] = math.cos(a)
        n[:, bx+10:bx+16, 0] = -math.sin(a); n[:, bx+10:bx+16, 2] = math.cos(a)
    rng = np.random.default_rng(7)
    n += rng.normal(0, 0.06, n.shape).astype(np.float32)  # nmap texture noise
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    mask = np.ones((h, w), bool)
    cg = cell_gradient_deg100(n, mask)
    gbi = float(np.nanmedian(cg))
    L = mask.copy(); L[:, w//3:] = False
    R = mask.copy(); R[:, :2*w//3] = False
    print(f'SYNTHETIC-FLAT  GBI={gbi:6.2f} deg/100px  SWEEP_h={ang(mean_dir(n,L),mean_dir(n,R)):5.2f} deg')
    return gbi

if __name__ == '__main__':
    import os
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    synthetic_floor()
    ns = {}
    exec(open(sys.argv[2]).read(), ns)
    jobs = ns['JOBS']  # list of (rawdir, tag, regions, zscale, scan_y)
    print(f"{'arm':<10} {'region':<10} {'GBI':>7} {'SWEEPh':>7} {'SWEEPv':>7} {'GEOMBOW':>8} {'px':>8}")
    for rawdir, tag, regions, zscale, scan_y in jobs:
        for r in analyze(rawdir, tag, regions, out, zscale, scan_y):
            print(f'{r[0]:<10} {r[1]:<10} {r[2]:7.2f} {r[3]:7.2f} {r[4]:7.2f} {r[5]:8.4f} {r[6]:8d}')
