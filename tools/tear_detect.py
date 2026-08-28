#!/usr/bin/env python3
# tear_detect.py — TEAR detector against the ground-truth reference (2026-08-28).
#
# A tear is a pixel where the authored-plane + heightfield reference (the same
# reference refdiff_detect.py validated against Gil-Ad's verdict corpus) says a
# wall/floor surface exists on the ray, but the engine rendered
#   HOLE       : nothing at all (z16 == 0, background through the wall), or
#   SEETHROUGH : a surface well BEHIND the reference surface (a through-slit
#                onto a farther wall/floor: zeng - zref > max(0.35 u, 3 %)).
# Occluders in FRONT of the reference (ship, lamps, mirror objects) are not
# tears and are ignored. Pure-black pixels on reference-covered wall are also
# counted as the legacy cross-check ("black px" of the earlier campaign).
#
# Each tear pixel is CLASSIFIED by the reference planes in its 6-px
# neighbourhood: BASE (a wall plane and a floor plane both present),
# SEAM (two distinct wall planes), PANEL (one plane only).
#
# usage: tear_detect.py <rawroot> <tag> <bare_tag> [outdir]
#   rawroot/<tag>/      arm render dump (color.ppm, depth.z16, mat.u32, refplane.txt)
#   rawroot/<bare_tag>/ the --no-greets-displace reference dump (authored faces + hm)
import sys, os, glob
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refdiff_detect as rd

def detect(rawdir, baredir, tag, outdir, metrics, ampscale=1.0):
    img, z16, npk, matp, W, H, zscale = rd.load_raw(rawdir)
    cam, mats, faces = rd.parse_refplane(glob.glob(os.path.join(baredir, '*_refplane.txt'))[0])
    camA = rd.parse_refplane(glob.glob(os.path.join(rawdir, '*_refplane.txt'))[0])[0]
    assert np.allclose(cam['src'], camA['src'], atol=1e-4), 'camera drift between arms'
    hms = {m: rd.load_hm(baredir, m, mats[m]) for m in mats}
    zeng = (np.float32(0xFF80) - z16.astype(np.float32)) / zscale
    zeng[z16 == 0] = np.nan
    best = None
    for half in (0.5, 0.0):
        for ysign in (1.0, -1.0):
            nref_, zref, mm, dom, ok, pid = rd.build_reference(cam, faces, mats, hms, W, H, half, ysign, ampscale)
            cand = ok & np.isfinite(zeng) & np.isfinite(zref) & (mm > 0)
            res = float(np.nanmedian(np.abs(zref[cand] - zeng[cand]))) if cand.sum() > 1000 else np.inf
            if best is None or res < best[0]:
                best = (res, half, ysign, zref, mm, ok, pid, nref_)
    res, half, ysign, zref, mm, ok, pid, nref = best
    refok = ok & (mm > 0) & np.isfinite(zref)
    hole = refok & (z16 == 0)
    behind = np.where(np.isfinite(zeng) & np.isfinite(zref), zeng - zref, 0.0)
    # ray-adjusted threshold: a legitimate recession of up to ~0.30 u along the
    # plane normal stretches by 1/|n.r| along a grazing ray and is NOT a tear;
    # a through-slit shows a surface far behind (another wall, the floor, sky).
    src = cam['src']; M = cam['M']
    pxs = (np.arange(W, dtype=np.float64)[None, :] + half - cam['cx']) / cam['px']
    pys = ysign * (np.arange(H, dtype=np.float64)[:, None] + half - cam['cy']) / cam['py']
    Rw = (pxs[..., None] * M[0][None, None, :] + np.broadcast_to(pys[..., None], (H, W, 3)) * M[1][None, None, :] + M[2][None, None, :])
    Rn = Rw / np.maximum(np.linalg.norm(Rw, axis=-1, keepdims=True), 1e-9)
    cosr = np.abs(np.sum(nref * Rn, -1))
    thr = np.maximum(0.35, 0.30 / np.maximum(cosr, 0.05))
    # grazing gate: below cos 0.2 (>78 deg incidence) a sub-0.05u relief
    # offset stretches to whole screen columns; registration there is not a
    # slit statement, so see-through is only scored at cos >= 0.2.
    seethrough = refok & np.isfinite(zeng) & (behind > thr) & (cosr >= 0.2)
    # SILHOUETTE GUARD: where the REFERENCE itself has a depth discontinuity
    # (a wall's silhouette against a farther wall) the engine silhouette can
    # sit a few pixels off because of the plateau recession — that is a
    # registration offset, not a through-slit. A slit is far-depth INSIDE a
    # region where the reference is continuous, so see-through pixels within
    # 3 px of a reference discontinuity are excluded; holes (z==0) within 2 px
    # of the reference's outer boundary (silhouette against true background)
    # are excluded for the same reason.
    zr = np.nan_to_num(zref, nan=0.0)
    gzr = np.abs(np.gradient(zr, axis=0)) + np.abs(np.gradient(zr, axis=1))
    refdisc = ndi.binary_dilation((gzr > 0.3) & refok, iterations=3)
    seethrough &= ~refdisc
    hole &= ndi.binary_erosion(refok, iterations=2)
    # registration guard: if the arm/bare camera registration residual is
    # poor (median |dz| > 0.2 u — extreme close-ups where the reference hits a
    # different surface than the engine), see-through cannot be scored.
    see_scored = res <= 0.2
    if not see_scored: seethrough &= False
    tear = hole | seethrough
    black = refok & (img.max(axis=-1) < 8) & np.isfinite(zeng)
    # classify by neighbourhood planes
    r = 6
    st = np.ones((2*r+1, 2*r+1), bool)
    wallp = np.where(mm == 1, pid, -1); floorp = np.where(mm == 2, pid, -1)
    has_floor = ndi.maximum_filter(floorp, footprint=st) >= 0
    # count distinct wall planes in the window: max != min over valid wall pids
    wmax = ndi.maximum_filter(wallp, footprint=st)
    wmin = ndi.minimum_filter(np.where(wallp >= 0, wallp, 10**6), footprint=st)
    has_wall = wmax >= 0
    two_walls = has_wall & (wmax != wmin) & (wmin < 10**6)
    base = tear & has_wall & has_floor
    seam = tear & two_walls & ~base
    panel = tear & ~base & ~seam
    n = dict(tear=int(tear.sum()), hole=int(hole.sum()), see=int(seethrough.sum()),
             black=int(black.sum()), base=int(base.sum()), seam=int(seam.sum()), panel=int(panel.sum()))
    # per-plane tear counts (top 5)
    pl = []
    for p in np.unique(pid[tear]):
        if p < 0: continue
        pl.append((int((tear & (pid == p)).sum()), int(p), 'wall' if (mm[(pid == p)] == 1).any() else 'floor'))
    pl.sort(reverse=True)
    line = ('%s: TEAR %d (hole %d, seethrough %d%s) black %d | base %d seam %d panel %d | cam half=%.1f ys=%+.0f med|dz| %.4f | top planes %s' %
            (tag, n['tear'], n['hole'], n['see'], '' if see_scored else ' UNSCORED', n['black'], n['base'], n['seam'], n['panel'],
             half, ysign, res, ' '.join('%d:%s:%d' % (c, k, p) for c, p, k in pl[:5])))
    print(line); open(metrics, 'a').write(line + '\n')
    # overlay: magenta = hole, cyan = seethrough, dilated for visibility
    ov = img.copy().astype(np.float32)
    hm_ = ndi.binary_dilation(hole, iterations=2); sm_ = ndi.binary_dilation(seethrough, iterations=2)
    ov[hm_] = [255, 0, 255]; ov[sm_] = [0, 255, 255]
    out = Image.fromarray(ov.astype(np.uint8)); d = ImageDraw.Draw(out)
    d.rectangle([6, H-42, 900, H-6], fill=(0, 0, 0))
    d.text((12, H-36), 'TEARS %s  magenta=HOLE (nothing rasterised where the reference has wall/floor)  cyan=SEE-THROUGH (>0.35u behind)' % tag, fill=(255,255,255))
    d.text((12, H-20), 'tear %d (hole %d see %d) black %d | base %d seam %d panel %d' %
           (n['tear'], n['hole'], n['see'], n['black'], n['base'], n['seam'], n['panel']), fill=(255,255,255))
    p = os.path.join(outdir, 'tears_%s.png' % tag); out.save(p)
    np.save(os.path.join(outdir, 'tearmask_%s.npy' % tag), np.packbits(tear))
    detect.last = dict(hole=hole, see=seethrough, tear=tear, zref=zref, pid=pid, mm=mm, cam=cam, half=half, ysign=ysign, W=W, H=H, base=base, seam=seam, panel=panel, img=img)
    return n

if __name__ == '__main__':
    root, tag, bare = sys.argv[1], sys.argv[2], sys.argv[3]
    outdir = sys.argv[4] if len(sys.argv) > 4 else os.path.dirname(root.rstrip('/'))
    os.makedirs(outdir, exist_ok=True)
    detect(os.path.join(root, tag), os.path.join(root, bare), tag, outdir, os.path.join(outdir, 'metrics_tears.txt'))
