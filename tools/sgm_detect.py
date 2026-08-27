#!/usr/bin/env python3
# sgm_detect.py — SHADING-vs-GEOMETRY MISMATCH metric (2026-08-28).
#
# The bulge_detect GBI/SWEEP metrics compare normals ACROSS pixels, so a wall
# seen edge-on scores high on legitimate grout relief (a 64px cell spans many
# world-units of bevels at grazing). This tool instead compares, PER PIXEL, the
# G-buffer shading normal against the GEOMETRIC normal reconstructed from the
# depth plane (cross of view-space position gradients) — both in view space, so
# the comparison is viewpoint-invariant: a correct flat-ish block face has
# shading ≈ geometry away from smoothing edges regardless of view angle, while
# ROLLING vertex normals (the bulge) diverge from the flat geometry mid-face.
#
#   SGM(px) = angle( oct_decode(norm.u32), normalize(dP/dx × dP/dy) )
#
# Masks: wall matIDs only, ZPage16 sentinel out, depth-discontinuity ±2px out
# (silhouettes and grout walls' own edges produce garbage gradients).
# FOV self-calibration: the undisplaced wall is plane-flat, so the FOVX that
# minimizes its median SGM is the projection actually used; wrong FOV shows as
# an irreducible floor there.
import sys, os, math
import numpy as np
from PIL import Image, ImageDraw

WALL_IDS = {10, 40}

def load(rawdir):
    import glob
    ppm = glob.glob(os.path.join(rawdir, '*_color.ppm'))[0]
    base = ppm[:-len('color.ppm')]
    with open(ppm, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = map(int, line.split()); f.readline()
        img = np.frombuffer(f.read(w*h*3), np.uint8).reshape(h, w, 3)
    z = np.fromfile(base + 'depth.z16', np.uint16).reshape(h, w)
    n = np.fromfile(base + 'norm.u32', np.uint32).reshape(h, w)
    m = np.fromfile(base + 'mat.u32', np.uint32).reshape(h, w)
    return img, z, n, m, w, h

def oct_decode(packed):
    qx = (packed & 0xffff).astype(np.int16).astype(np.float32) / 32767.0
    qy = ((packed >> 16) & 0xffff).astype(np.int16).astype(np.float32) / 32767.0
    az = 1.0 - np.abs(qx) - np.abs(qy)
    fold = az < 0
    fx = (1.0 - np.abs(qy)) * np.where(qx >= 0, 1.0, -1.0)
    fy = (1.0 - np.abs(qx)) * np.where(qy >= 0, 1.0, -1.0)
    ox = np.where(fold, fx, qx); oy = np.where(fold, fy, qy)
    n = np.stack([ox, oy, az], -1)
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)

def sgm(rawdir, fovx_persp, zscale, regions, outdir, tag, arrow=True):
    img, zenc, npk, mat, W, H = load(rawdir)
    mid = ((mat >> 20) & 0xFF)
    wall = np.isin(mid, list(WALL_IDS)) & (zenc != 0)
    zv = (np.float32(0xFF80) - zenc.astype(np.float32)) / zscale
    zv[zenc == 0] = np.nan
    cx, cy = W/2.0, H/2.0
    persp_y = fovx_persp * (4.0/3.0) * (H/float(W))
    xs = (np.arange(W)[None, :] - cx) * zv / fovx_persp
    ys = (cy - np.arange(H)[:, None]) * zv / persp_y
    P = np.stack([xs, ys, zv], -1)
    dPy, dPx = np.gradient(P, axis=(0, 1))
    g = np.cross(dPx, dPy)
    g /= np.maximum(np.linalg.norm(g, axis=-1, keepdims=True), 1e-12)
    sN = oct_decode(npk)
    # orient geometric normal with the shading field's hemisphere per-pixel
    flip = (np.sum(g * sN, -1) < 0)
    g[flip] *= -1
    # depth-discontinuity mask: kill ±2px around big z steps (silhouette/grout edge)
    dz = np.maximum(np.abs(np.gradient(zv, axis=0)), np.abs(np.gradient(zv, axis=1)))
    disc = dz > (zv * 0.004 + 0.008)
    from scipy import ndimage as ndi
    bad = ndi.binary_dilation(disc, iterations=2)
    ok = wall & ~bad & np.isfinite(zv)
    dot = np.clip(np.sum(g * sN, -1), -1, 1)
    ang = np.degrees(np.arccos(dot))
    rows = []
    ov = Image.fromarray(img).convert('RGB'); d = ImageDraw.Draw(ov)
    for name, (x0, y0, x1, y1) in regions.items():
        m = ok[y0:y1, x0:x1]
        a = ang[y0:y1, x0:x1][m]
        a = a[np.isfinite(a)]   # gradient at a finite/NaN boundary is NaN
        med = float(np.median(a)) if a.size else float('nan')
        p90 = float(np.percentile(a, 90)) if a.size else float('nan')
        rows.append((tag, name, med, p90, int(a.size)))
        # heat overlay: color coded SGM per 8px
        for ay in range(y0, y1, 8):
            for ax_ in range(x0, x1, 8):
                if not ok[ay, ax_]: continue
                v = ang[ay, ax_]
                if v > 25: col = (255, 40, 40)
                elif v > 12: col = (255, 140, 0)
                elif v > 6: col = (250, 240, 60)
                else: col = (60, 220, 60)
                d.rectangle([ax_, ay, ax_+2, ay+2], fill=col)
        d.rectangle([x0, y0, x1, y1], outline=(255, 0, 255))
        d.text((x0+4, y0+4), name, fill=(255, 0, 255))
    ov.save(os.path.join(outdir, f'sgm_{tag}.png'))
    return rows

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    ns = {}
    exec(open(sys.argv[2]).read(), ns)
    print(f"{'arm':<12} {'region':<10} {'SGMmed':>7} {'SGMp90':>7} {'px':>9}")
    for rawdir, tag, regions, zscale, fovx in ns['JOBS']:
        for r in sgm(rawdir, fovx, zscale, regions, out, tag):
            print(f'{r[0]:<12} {r[1]:<10} {r[2]:7.2f} {r[3]:7.2f} {r[4]:9d}')
