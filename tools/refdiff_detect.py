#!/usr/bin/env python3
# refdiff_detect.py — GROUND-TRUTH REFERENCE detector (2026-08-28).
#
# Every prior bulge metric (black-px, GBI/SWEEP, SGM) compared the engine to
# ITSELF (its own depth, its own neighbors) and each passed states Gil-Ad
# judges obviously wrong. This tool compares the engine against a reference
# that is correct BY CONSTRUCTION and shares no machinery with the bake:
#
#   reference = authored wall plane (coarse FACE dump from a
#               --no-greets-displace run: A/B/C world pos + per-corner UVs)
#             + the height map the bake samples (deswizzled mip, same
#               bilinear texel-center + toroidal wrap convention)
#             + the bake's own amp/mipMean scalars
#   per pixel: ray-cast the authored triangles, iterate the hit onto the
#              displaced surface S = P + N̂·amp·(h(u,v)−mean), and take the
#              analytic surface normal from the bilinear height gradient.
#
# Engine field: greets_t*_norm.u32 (oct16.16 VIEW-space G-buffer shading
# normal, pre-normal-map) rotated to world by the dumped camera matrix.
# POST-normal-map shading is NOT reachable from the dumps — what this tool
# cannot see is bounded to the nmap/POM overlay (per-texel detail; it cannot
# hide a block/panel-scale bulge in the pre-nmap field, but a march-side
# low-frequency error would need its own instrument).
#
# Two diff bands per pixel (angle engine-vs-reference, degrees):
#   RAW — full-band. Includes legitimate frequency-split residue (the engine
#         carries fine relief in the nmap, not the vertex field) — context.
#   LF  — both fields Gaussian-blurred (σ=16px, mask-normalized) first.
#         The bulge is a block/panel-scale defect; LF is the verdict band.
# Plus DEPTH residual vs the reference displaced surface (world units) —
# the geometry-vs-reference check (a true-geometry bow shows here).
#
# Verdict region: pixels of wall matIDs on DOMINANT planes (planes holding
# >30 u² of authored area). The curved wall's small per-segment planes are
# reported separately: authored intent there is smooth shading, which a
# faceted plane reference cannot honestly score.
#
# Camera conventions (pixel-center offset, y sign) are AUTO-VALIDATED on the
# undisplaced bare arm: the choice must drive the bare wall's median |depth
# residual| to the depth quantum (~0.0025 u at FZP=150); the pick and the
# residual are printed — a wrong camera cannot hide.
import sys, os, math, glob, re
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi

WALL_IDS = {10, 40}
FLOOR_IDS = {8, 41}
LF_SIGMA = 16.0
DOMINANT_AREA = 30.0

def load_ppm(p):
    with open(p, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = map(int, line.split()); f.readline()
        return np.frombuffer(f.read(w*h*3), np.uint8).reshape(h, w, 3), w, h

def load_raw(rawdir):
    ppm = glob.glob(os.path.join(rawdir, '*_color.ppm'))[0]
    base = ppm[:-len('color.ppm')]
    img, w, h = load_ppm(ppm)
    z = np.fromfile(base + 'depth.z16', np.uint16).reshape(h, w)
    n = np.fromfile(base + 'norm.u32', np.uint32).reshape(h, w)
    m = np.fromfile(base + 'mat.u32', np.uint32).reshape(h, w)
    zscale = 395.6364
    log = os.path.join(rawdir, 'stderr.log')
    if os.path.exists(log):
        t = open(log, errors='ignore').read()
        mm = re.search(r'zscale=([0-9.]+)', t)
        if mm: zscale = float(mm.group(1))
    return img, z, n, m, w, h, zscale

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

def parse_refplane(path):
    cam = {}; mats = {}; faces = []
    M = np.zeros((3,3), np.float64)
    for line in open(path):
        t = line.split()
        if not t: continue
        if t[0] == 'CAM':
            cam = dict(src=np.array(list(map(float, t[2:5]))),
                       px=float(t[6]), py=float(t[7]),
                       cx=float(t[9]), cy=float(t[10]),
                       W=int(t[12]), H=int(t[13]), fov=float(t[15]))
        elif t[0].startswith('CAMMAT'):
            M[int(t[0][-1])] = list(map(float, t[1:4]))
        elif t[0] == 'MATINFO' and len(t) > 3:
            mats[t[1]] = dict(sx=int(t[5]), sy=int(t[6]),
                              useMip=int(t[11]), amp=float(t[13]),
                              mean=float(t[17]))
        elif t[0] == 'FACE':
            mesh, mat = t[1], t[2]
            if mesh.startswith('__mirrorClone'): continue
            v = list(map(float, t[3:]))
            faces.append((mesh, mat, np.array(v[0:3]), np.array(v[3:6]),
                          np.array(v[6:9]), np.array(v[9:15]), np.array(v[15:18])))
    cam['M'] = M
    return cam, mats, faces

def load_hm(rawdir, mat, mi):
    safe = mat.replace(':', '_').replace('/', '_')
    p = os.path.join(rawdir, 'greets_hm_%s_mip%d.u8' % (safe, mi['useMip']))
    mw = max(1, mi['sx'] >> mi['useMip']); mh = max(1, mi['sy'] >> mi['useMip'])
    return np.fromfile(p, np.uint8).reshape(mh, mw).astype(np.float32) / 255.0

def bilerp_grad(hm, u, v):
    mh, mw = hm.shape
    x = u * mw - 0.5; y = v * mh - 0.5
    x0 = np.floor(x); y0 = np.floor(y)
    fx = (x - x0).astype(np.float32); fy = (y - y0).astype(np.float32)
    xi0 = x0.astype(np.int64) % mw; xi1 = (xi0 + 1) % mw
    yi0 = y0.astype(np.int64) % mh; yi1 = (yi0 + 1) % mh
    h00 = hm[yi0, xi0]; h10 = hm[yi0, xi1]; h01 = hm[yi1, xi0]; h11 = hm[yi1, xi1]
    h = (h00*(1-fx) + h10*fx)*(1-fy) + (h01*(1-fx) + h11*fx)*fy
    dhdu = (((h10-h00)*(1-fy)) + (h11-h01)*fy) * mw
    dhdv = (((h01-h00)*(1-fx)) + (h11-h10)*fx) * mh
    return h, dhdu, dhdv

def raycast(cam, faces, mats, hms, W, H, half, ysign):
    src = cam['src']; M = cam['M']
    pxs = (np.arange(W, dtype=np.float64)[None, :] + half - cam['cx']) / cam['px']
    pys = ysign * (np.arange(H, dtype=np.float64)[:, None] + half - cam['cy']) / cam['py']
    # view dir (x, y, 1) -> world via rows-basis M
    Rw = (pxs[..., None] * M[0][None, None, :] +
          np.broadcast_to(pys[..., None], (H, W, 3)) * M[1][None, None, :] +
          M[2][None, None, :])
    tbest = np.full((H, W), np.inf)
    fid = np.full((H, W), -1, np.int32)
    finfo = []
    for i, (mesh, mat, A, B, C, uv, N) in enumerate(faces):
        e1 = B - A; e2 = C - A
        n = np.cross(e1, e2)
        nl = np.linalg.norm(n)
        if nl < 1e-9: finfo.append(None); continue
        nu = n / nl
        # orient to the VISIBLE side (the winding cross is the anti-visible
        # normal in this engine): the wall's outward normal opposes the
        # camera->face vector. Displacement rides the visible side.
        if nu @ (A - src) > 0: nu = -nu
        # screen bbox from projected verts (all three in front)
        vs = np.stack([A, B, C]) - src
        Vv = vs @ M.T
        if np.any(Vv[:, 2] < 0.05): finfo.append((A, e1, e2, nu, uv, mat, None)); continue
        sx = cam['cx'] + Vv[:, 0] / Vv[:, 2] * cam['px'] - half
        sy = cam['cy'] + ysign * (Vv[:, 1] / Vv[:, 2]) * cam['py'] - half
        x0 = max(0, int(np.floor(sx.min())) - 1); x1 = min(W - 1, int(np.ceil(sx.max())) + 1)
        y0 = max(0, int(np.floor(sy.min())) - 1); y1 = min(H - 1, int(np.ceil(sy.max())) + 1)
        finfo.append((A, e1, e2, nu, uv, mat, None))
        if x1 < x0 or y1 < y0: continue
        R = Rw[y0:y1+1, x0:x1+1]
        denom = R @ nu
        with np.errstate(divide='ignore', invalid='ignore'):
            t = ((A - src) @ nu) / denom
        p = src + t[..., None] * R
        w = p - A
        d11 = e1 @ e1; d12 = e1 @ e2; d22 = e2 @ e2
        det = d11 * d22 - d12 * d12
        if abs(det) < 1e-12: continue
        w1 = w @ e1; w2 = w @ e2
        a = (d22 * w1 - d12 * w2) / det
        b = (d11 * w2 - d12 * w1) / det
        eps = 1e-4
        hit = (t > 0.1) & np.isfinite(t) & (a >= -eps) & (b >= -eps) & (a + b <= 1 + eps)
        sub_t = tbest[y0:y1+1, x0:x1+1]
        upd = hit & (t < sub_t)
        sub_t[upd] = t[upd]
        fid[y0:y1+1, x0:x1+1][upd] = i
    return Rw, tbest, fid, finfo

def build_reference(cam, faces, mats, hms, W, H, half, ysign, ampscale=1.0):
    src = cam['src']
    Rw, t, fid, finfo = raycast(cam, faces, mats, hms, W, H, half, ysign)
    ok = fid >= 0
    nref = np.zeros((H, W, 3), np.float32)
    zref = np.full((H, W), np.nan)
    plane_id = np.full((H, W), -1, np.int32)
    matmask = np.zeros((H, W), np.uint8)  # 1 wall 2 floor
    # per-face plane cluster
    keys = {}
    areas = {}
    fkey = []
    for f in faces:
        A, B, C = f[2], f[3], f[4]
        n = np.cross(B - A, C - A); nl = np.linalg.norm(n)
        k = tuple(np.round(n / max(nl, 1e-9), 3)) + (round(float(n @ A / max(nl, 1e-9)), 2),)
        if k not in keys: keys[k] = len(keys); areas[keys[k]] = 0.0
        areas[keys[k]] += nl * 0.5
        fkey.append(keys[k])
    for i, f in enumerate(faces):
        if finfo[i] is None: continue
        A, e1, e2, nu, uv, mat, _ = finfo[i]
        sel = fid == i
        if not sel.any(): continue
        mi = mats.get(mat); hm = hms.get(mat)
        R = Rw[sel]
        tt = t[sel]
        duv1 = uv[2:4] - uv[0:2]; duv2 = uv[4:6] - uv[0:2]
        det = duv1[0] * duv2[1] - duv2[0] * duv1[1]
        if mi is None or hm is None or abs(det) < 1e-12:
            nref[sel] = nu; zref[sel] = tt
            plane_id[sel] = fkey[i]
            matmask[sel] = 1 if mat.startswith('rooms') else 2
            continue
        dPdu = (e1 * duv2[1] - e2 * duv1[1]) / det
        dPdv = (e2 * duv1[0] - e1 * duv2[0]) / det
        d11 = e1 @ e1; d12 = e1 @ e2; d22 = e2 @ e2
        fdet = d11 * d22 - d12 * d12
        amp = mi['amp'] * ampscale; mean = mi['mean']
        dn = R @ nu
        for _ in range(3):
            p = cam['src'] + tt[:, None] * R
            w = p - A
            a = (d22 * (w @ e1) - d12 * (w @ e2)) / fdet
            b = (d11 * (w @ e2) - d12 * (w @ e1)) / fdet
            u = uv[0] + a * duv1[0] + b * duv2[0]
            v = uv[1] + a * duv1[1] + b * duv2[1]
            h, gu, gv = bilerp_grad(hm, u, v)
            d = amp * (h - mean)
            tt = ((A - cam['src']) @ nu + d) / dn
        Su = dPdu[None, :] + nu[None, :] * (amp * gu)[:, None]
        Sv = dPdv[None, :] + nu[None, :] * (amp * gv)[:, None]
        nr = np.cross(Su, Sv)
        nr /= np.maximum(np.linalg.norm(nr, axis=-1, keepdims=True), 1e-12)
        flip = (nr @ nu) < 0
        nr[flip] *= -1
        nref[sel] = nr
        zref[sel] = tt
        plane_id[sel] = fkey[i]
        matmask[sel] = 1 if mat.startswith('rooms') else 2
    dominant = {k for k, ar in areas.items() if ar > DOMINANT_AREA}
    dom = np.isin(plane_id, list(dominant))
    return nref, zref, matmask, dom, ok, plane_id

def masked_blur(vec, mask, sigma):
    m = mask.astype(np.float32)
    out = np.zeros_like(vec)
    mb = ndi.gaussian_filter(m, sigma)
    for c in range(3):
        out[..., c] = ndi.gaussian_filter(vec[..., c] * m, sigma) / np.maximum(mb, 1e-6)
    out /= np.maximum(np.linalg.norm(out, axis=-1, keepdims=True), 1e-9)
    return out

def run_arm(rawdir, baredir, tag, outdir, metrics):
    # the reference models what the ARM should render: displaced arms carry
    # the bake's amp; the bare arm is the authored flat plane (amp=0).
    ampscale = 0.0 if tag.startswith('bare') else 1.0
    img, z16, npk, matp, W, H, zscale = load_raw(rawdir)
    cam, mats, faces = parse_refplane(glob.glob(os.path.join(baredir, '*_refplane.txt'))[0])
    # arm camera must equal bare camera
    camA = parse_refplane(glob.glob(os.path.join(rawdir, '*_refplane.txt'))[0])[0]
    assert np.allclose(cam['src'], camA['src'], atol=1e-4), 'camera drift between arms'
    hms = {m: load_hm(baredir if os.path.exists(os.path.join(
        baredir, 'greets_hm_%s_mip%d.u8' % (m.replace(':', '_'), mats[m]['useMip'])))
        else rawdir, m, mats[m]) for m in mats}
    zeng = (np.float32(0xFF80) - z16.astype(np.float32)) / zscale
    zeng[z16 == 0] = np.nan
    mid = ((matp >> 20) & 0xFF)
    best = None
    for half in (0.5, 0.0):
        for ysign in (1.0, -1.0):
            nref, zref, mm, dom, ok, pid = build_reference(cam, faces, mats, hms, W, H, half, ysign, ampscale)
            cand = ok & np.isfinite(zeng) & np.isfinite(zref) & (mm > 0)
            if cand.sum() < 1000: res = np.inf
            else: res = float(np.nanmedian(np.abs(zref[cand] - zeng[cand])))
            if best is None or res < best[0]:
                best = (res, half, ysign, nref, zref, mm, dom, ok, pid)
    res, half, ysign, nref, zref, mm, dom, ok, pid = best
    print('[%s] camera pick: half=%.1f ysign=%+.0f  median|dz|=%.4f u' % (tag, half, ysign, res))
    nv = oct_decode(npk)
    neng = nv @ cam['M']  # view->world (rows basis)
    dz = zref - zeng
    depth_ok = ok & np.isfinite(zeng) & np.isfinite(zref) & (np.abs(dz) < 0.25)
    wall_ids = np.isin(mid, list(WALL_IDS))
    dzg = np.abs(np.gradient(zeng, axis=0)) + np.abs(np.gradient(zeng, axis=1))
    disc = ndi.binary_dilation(dzg > (zeng * 0.004 + 0.008), iterations=2)
    valid = depth_ok & wall_ids & ~disc & (mm == 1)
    verdict = valid & dom
    curved = valid & ~dom
    dote = np.clip(np.sum(nref * neng, -1), -1, 1)
    raw = np.degrees(np.arccos(dote))
    nrl = masked_blur(nref, valid, LF_SIGMA)
    nel = masked_blur(neng, valid, LF_SIGMA)
    lf = np.degrees(np.arccos(np.clip(np.sum(nrl * nel, -1), -1, 1)))
    # GEOMETRY channel (his marked-jut class): per-pixel dz vs the OUTER
    # displaced envelope (nearest-hit across both planes' displaced surfaces
    # IS the mitre envelope bound). NOT masked by depth agreement — that mask
    # would erase exactly the protrusions being hunted. dz>0 = engine surface
    # in FRONT of the envelope (protruding jut); dz<0 = recessed.
    geo_ok = ok & wall_ids & np.isfinite(zeng) & np.isfinite(zref)
    if tag.endswith('_A'):
        yy, xx = np.mgrid[0:H, 0:W]
        rect = (xx >= 1080) & (xx <= 1235) & (yy >= 315) & (yy <= 415)
        ell = (((xx - 1160) / 130.0) ** 2 + ((yy - 650) / 115.0) ** 2) <= 1.0
        for rname, rm in (('MARK-rect', rect), ('MARK-ellipse', ell)):
            m = rm & geo_ok
            ml = rm & valid
            if m.sum():
                gs = ('%s %s: geo n=%d dz med %+.4f p90|dz| %.4f frac(dz>+0.08) %.1f%% '
                      'frac(|dz|>0.08) %.1f%% ; LF med %.2f (n=%d)' %
                      (tag, rname, int(m.sum()), float(np.median(dz[m])),
                       float(np.percentile(np.abs(dz[m]), 90)),
                       100.0 * float((dz[m] > 0.08).mean()),
                       100.0 * float((np.abs(dz[m]) > 0.08).mean()),
                       float(np.median(lf[ml])) if ml.sum() > 50 else float('nan'),
                       int(ml.sum())))
            else:
                gs = '%s %s: geo n=0 (region not covered by reference!)' % (tag, rname)
            print(gs); open(metrics, 'a').write(gs + '\n')
    # geometry heatmap: signed dz over the render (red = protrudes past the
    # envelope, blue = recessed), saturating at 0.15 u
    gv = np.clip(dz / 0.15, -1, 1)
    ovg = img.copy().astype(np.float32)
    tintg = np.zeros_like(ovg)
    tintg[..., 0] = np.where(gv > 0, 255 * gv, 0)
    tintg[..., 2] = np.where(gv < 0, 255 * -gv, 0)
    ag = np.where(geo_ok, np.clip(np.abs(gv), 0, 1) * 0.85, 0.0)[..., None]
    ovg = ovg * (1 - ag) + tintg * ag
    outg = Image.fromarray(ovg.astype(np.uint8))
    dg = ImageDraw.Draw(outg)
    dg.rectangle([6, H-42, 760, H-6], fill=(0, 0, 0))
    dg.text((12, H-36), 'REFDIFF-GEO %s  signed depth vs displaced-envelope reference' % tag, fill=(255,255,255))
    dg.text((12, H-20), 'red = protrudes past outer mitre envelope, blue = recessed; full = 0.15 u', fill=(255,255,255))
    if tag.endswith('_A'):
        dg.rectangle([1080, 315, 1235, 415], outline=(255, 255, 0), width=3)
        dg.ellipse([1160-130, 650-115, 1160+130, 650+115], outline=(255, 255, 0), width=3)
    pg = os.path.join(outdir, 'refdiff_geo_%s.png' % tag)
    outg.save(pg)
    def stats(msk):
        if msk.sum() < 500: return (np.nan,)*5
        return (float(np.median(raw[msk])), float(np.percentile(raw[msk], 90)),
                float(np.median(lf[msk])), float(np.percentile(lf[msk], 90)),
                float(np.median(dz[msk])))
    sv = stats(verdict); sc = stats(curved)
    # per-plane localization: the verdict statistic is the WORST dominant
    # plane, not the global median (a local defect must not drown in clean
    # wall). Top offenders by LF p90.
    plines = []
    for p in np.unique(pid[verdict]):
        msk = verdict & (pid == p)
        if msk.sum() < 5000: continue
        plines.append((float(np.percentile(lf[msk], 90)),
                       float(np.median(lf[msk])), int(msk.sum()), int(p)))
    plines.sort(reverse=True)
    worst = plines[0] if plines else (np.nan, np.nan, 0, -1)
    for pl in plines[:5]:
        w = '  plane %d n=%d LF med %.2f p90 %.2f' % (pl[3], pl[2], pl[1], pl[0])
        print(w); open(metrics, 'a').write(w + '\n')
    line = ('%s: cam(half=%.1f,ys=%+.0f) planar[n=%d] raw %.2f/%.2f LF %.2f/%.2f dz %.4f | '
            'curved[n=%d] raw %.2f/%.2f LF %.2f/%.2f dz %.4f | WORSTPLANE %d LF med %.2f p90 %.2f' %
            (tag, half, ysign, int(verdict.sum()), sv[0], sv[1], sv[2], sv[3], sv[4],
             int(curved.sum()), sc[0], sc[1], sc[2], sc[3], sc[4],
             worst[3], worst[1], worst[0]))
    print(line)
    open(metrics, 'a').write(line + '\n')
    # heatmap: LF angle over the render
    ov = img.copy().astype(np.float32)
    hot = np.clip(lf / 15.0, 0, 1)
    red = np.zeros_like(ov); red[..., 0] = 255
    grn = np.zeros_like(ov); grn[..., 1] = 255
    tint = grn * (1 - hot[..., None]) + red * hot[..., None]
    aow = np.where(valid, 0.45, 0.0)[..., None]
    ov = ov * (1 - aow) + tint * aow
    out = Image.fromarray(ov.astype(np.uint8))
    d = ImageDraw.Draw(out)
    d.rectangle([6, H-58, 690, H-6], fill=(0, 0, 0))
    d.text((12, H-52), 'REFDIFF %s  LF angle vs ground-truth plane+heightfield reference' % tag, fill=(255,255,255))
    d.text((12, H-36), 'green 0deg -> red >=15deg (sigma=%.0fpx band) ; planar med/p90 %.2f/%.2f  curved %.2f/%.2f' %
           (LF_SIGMA, sv[2], sv[3], sc[2], sc[3]), fill=(255,255,255))
    d.text((12, H-20), 'unmasked = not scoreable (occluder/floor/mirror/silhouette); dz med %.4f u' % sv[4], fill=(180,180,180))
    p = os.path.join(outdir, 'refdiff_%s.png' % tag)
    out.save(p)
    print('  ->', p)
    return sv, sc

if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else 'docs/img/refdiff/raw'
    outdir = os.path.dirname(root.rstrip('/'))
    metrics = os.path.join(outdir, 'metrics_refdiff.txt')
    open(metrics, 'a').write('--- run ---\n')
    for cam in ('A', 'B'):
        bare = os.path.join(root, 'bare_' + cam)
        for arm in ('bare', 'r3', 'ship', 'fixcam', 'prefo'):
            d = os.path.join(root, '%s_%s' % (arm, cam))
            if os.path.isdir(d):
                run_arm(d, bare, '%s_%s' % (arm, cam), outdir, metrics)
