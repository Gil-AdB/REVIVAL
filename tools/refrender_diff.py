#!/usr/bin/env python3
"""Per-pixel comparison of the TESSELLATED bake against the REFERENCE relief render.

The reference (FDS/RENDER/DeferredDisplaceRef.cpp, --greets_displace_ref) renders
the DEFINITION of the displaced stone surface with no tessellation.  This tool
answers, per stone pixel, how far the bake's surface is from it:

  * depth      |dz| in WORLD units along the view ray, and the share of stone
               pixels past 0.02 u and 0.08 u
  * normal     the angle between the two shading normals (view space)
  * cracks     pixels where one arm has a surface and the other has none

and writes three maps: a signed dz map, a normal-angle map, and a
LAPLACIAN-WEIGHTED |difference| map.  The last one is Shafiee Sarvestani et al.
(2024)'s trick: weighting the absolute difference by the local Laplacian
suppresses the smooth, uniform offsets that a plain |dz| map is dominated by and
leaves the STRUCTURAL disagreement -- edges in the wrong place, a course line
half a block off, a crack -- which is what the eye actually objects to.

Inputs per arm are what tools/refrender_battery.sh already writes:
  <dir>/greets_t<NNNNNN>_depth.z16   uint16 ZPage16              (FDS_SNAPSHOT_ZDUMP)
  <dir>/greets_t<NNNNNN>_mat.u32     uint32 mip:4|matID:8|suv:20 (FDS_SNAPSHOT_GBUFDUMP)
  <dir>/greets_t<NNNNNN>_nrm.u32     uint32 octahedral 16.16     (FDS_SNAPSHOT_NRMDUMP)
  <dir>/ref.bin                      the reference's own raw dump (FDS_REFRENDER_DUMP)

usage:
  tools/refrender_diff.py --tess DIR --ref DIR [--bare DIR] [--zscale F]
                          [--out-prefix PATH] [--json]
"""

import argparse, glob, json, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from provenance import carry_through


# ── the engine's octahedral 16.16 codec (meka::oct_decode_u32) ──────────────
def oct_decode_u32(packed):
    qx = (packed & 0xFFFF).astype(np.int32)
    qy = ((packed >> 16) & 0xFFFF).astype(np.int32)
    qx = np.where(qx >= 32768, qx - 65536, qx)
    qy = np.where(qy >= 32768, qy - 65536, qy)
    ox = qx.astype(np.float64) / 32767.0
    oy = qy.astype(np.float64) / 32767.0
    az = 1.0 - np.abs(ox) - np.abs(oy)
    neg = az < 0.0
    fx = (1.0 - np.abs(oy)) * np.where(ox >= 0.0, 1.0, -1.0)
    fy = (1.0 - np.abs(ox)) * np.where(oy >= 0.0, 1.0, -1.0)
    ox = np.where(neg, fx, ox)
    oy = np.where(neg, fy, oy)
    n = np.stack([ox, oy, az], axis=-1)
    ln = np.linalg.norm(n, axis=-1, keepdims=True)
    return n / np.maximum(ln, 1e-30)


def find(d, suffix):
    g = sorted(glob.glob(os.path.join(d, "greets_t*_" + suffix)))
    if not g:
        sys.exit("missing %s in %s" % (suffix, d))
    return g[0]


def load_arm(d, w, h):
    z = np.fromfile(find(d, "depth.z16"), dtype=np.uint16)[: w * h].reshape(h, w)
    m = np.fromfile(find(d, "mat.u32"), dtype=np.uint32)[: w * h].reshape(h, w)
    npath = os.path.join(d, os.path.basename(find(d, "mat.u32")).replace("mat.u32", "nrm.u32"))
    n = (oct_decode_u32(np.fromfile(npath, dtype=np.uint32)[: w * h].reshape(h, w))
         if os.path.exists(npath) else None)
    return z, m, n


def load_refbin(path, w, h):
    """REFRND01 = z | n | faceId | flags;  REFRND02 adds the crease-dh plane."""
    with open(path, "rb") as f:
        magic = f.read(8)
        assert magic in (b"REFRND01", b"REFRND02"), "not a REFRND0x dump: " + path
        rw, rh = np.frombuffer(f.read(8), dtype=np.int32)
        assert (int(rw), int(rh)) == (w, h), "resolution mismatch"
        n = w * h
        z = np.frombuffer(f.read(n * 4), dtype=np.float32).reshape(h, w).astype(np.float64)
        nrm = np.frombuffer(f.read(n * 12), dtype=np.float32).reshape(h, w, 3).astype(np.float64)
        fid = np.frombuffer(f.read(n * 4), dtype=np.int32).reshape(h, w)
        fl = np.frombuffer(f.read(n * 4), dtype=np.uint32).reshape(h, w)
    return z, nrm, fid, fl


def laplacian(a):
    p = np.pad(a, 1, mode="edge")
    return (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:] - 4.0 * a)


def colormap(v, lo, hi, cmap="magma"):
    t = np.clip((v - lo) / max(1e-12, hi - lo), 0.0, 1.0)
    if cmap == "diverging":                       # blue -> white -> red
        r = np.clip(2.0 * t, 0, 1)
        b = np.clip(2.0 * (1.0 - t), 0, 1)
        g = 1.0 - np.abs(2.0 * t - 1.0)
        rgb = np.stack([r, g, b], -1)
    else:                                          # dark -> orange -> white
        r = np.clip(t * 2.2, 0, 1)
        g = np.clip(t * 1.6 - 0.35, 0, 1)
        b = np.clip(t * 2.4 - 1.5, 0, 1)
        rgb = np.stack([r, g, b], -1)
    return (rgb * 255.0 + 0.5).astype(np.uint8)


# Every map this tool writes is derived from the ARM DIRECTORIES, whose
# provenance lives on the colour PPM the same snapshot run dumped there. Pass
# those PPMs as the inputs so a _dz.png names the two arms it compares.
_PROV_INPUTS = []


def arm_provenance_source(d):
    """The provenance-bearing file in a snapshot arm dir (its colour PPM)."""
    g = sorted(glob.glob(os.path.join(d, "greets_t*_color.ppm")))
    return g[0] if g else None


def save_png(path, rgb):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow needed for the map images")
    Image.fromarray(rgb, "RGB").save(path)
    if _PROV_INPUTS:
        carry_through(path, "tools/refrender_diff.py", _PROV_INPUTS)


def pct(a, q):
    return float(np.percentile(a, q)) if a.size else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tess", required=True, help="tessellated-bake arm dir")
    ap.add_argument("--ref", required=True, help="reference arm dir (has ref.bin)")
    ap.add_argument("--bare", help="undisplaced arm dir (optional, for context)")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--zscale", type=float, default=None,
                    help="viewZ = (0xFF80 - z16)/zscale; read off the [REFRENDER] banner")
    ap.add_argument("--out-prefix")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--stone-mat", help="comma-separated stone matIDs; default: read "
                                        "them from the reference arm's log.txt banner")
    ap.add_argument("--flat-deg", type=float, default=2.0,
                    help="BLOCK-INTERIOR mask: a reference pixel counts as block "
                         "interior when its shading normal is within this many "
                         "degrees of its own authored face's plane normal, which "
                         "is estimated per faceId as the median of the reference "
                         "normals on that face. The flat block tops and the flat "
                         "mortar floors pass; the groove walls and shoulders, "
                         "where the relief's own gradient tilts the normal, do "
                         "not. Needs no camera and no second reference arm.")
    ap.add_argument("--crease-band", type=int, default=6,
                    help="CREASE-BAND mask radius in pixels, 0 to disable. The "
                         "complement of --flat-deg's question: P4 (design §2e) "
                         "moves the JUNCTION RINGS and nothing else, so a "
                         "block-interior number cannot see it at all. A pixel "
                         "is on a crease boundary when a 4-neighbour belongs to "
                         "a different reference faceId whose plane normal "
                         "differs by more than --crease-deg; the band is that "
                         "set dilated by this radius. Same reference, same "
                         "faceId plane table as the flat mask, so the two "
                         "masks are two questions about one render.")
    ap.add_argument("--crease-deg", type=float, default=10.0,
                    help="how far two faceId plane normals must differ for the "
                         "boundary between them to count as a crease rather "
                         "than a same-plane seam (the chart budget, 10°)")
    a = ap.parse_args()
    w, h = a.width, a.height

    # zScale: the reference's own dump carries WORLD z and its arm's z16 carries
    # the same surface, so the two together give the scalar exactly -- no need to
    # guess it, and no chance of deriving it circularly from an assumed match.
    rz, rn, rfid, rfl = load_refbin(os.path.join(a.ref, "ref.bin"), w, h)
    rz16, rmat, rnrm = load_arm(a.ref, w, h)
    zscale = a.zscale
    if zscale is None:
        m = (rfl & 1).astype(bool) & (rz16 > 0) & (rz > 0.5)
        if m.sum() < 1000:
            sys.exit("cannot solve zscale: too few reference pixels")
        zscale = float(np.median((65408.0 - rz16[m].astype(np.float64)) / rz[m]))

    tz16, tmat, tnrm = load_arm(a.tess, w, h)
    tz = np.where(tz16 > 0, (65408.0 - tz16.astype(np.float64)) / zscale, 0.0)

    # THE DOMAIN IS STONE-VS-STONE.  Comparing "the reference has a surface" to
    # "the tessellated arm has any surface at all" counts the robot, the ceiling
    # and every decal as bake-only surface -- 197k pixels of it at cam A, which
    # says nothing about displacement.  The stone matIDs come from the
    # reference's own banner (it prints one [REFRENDER-MAT] line per stone
    # material with its matID), so the two arms are asked the same question.
    stone_ids = set()
    if a.stone_mat:
        stone_ids = {int(x) for x in a.stone_mat.split(",") if x.strip()}
    else:
        logp = os.path.join(a.ref, "log.txt")
        if os.path.exists(logp):
            for line in open(logp, errors="ignore"):
                if "[REFRENDER-MAT]" in line and "matID=" in line:
                    stone_ids.add(int(line.split("matID=")[1].split()[0]))
    if not stone_ids:
        sys.exit("no stone matIDs: pass --stone-mat, or run the ref arm with "
                 "--greets_displace_ref_stats=1 so log.txt carries the banner")

    tmatid = ((tmat >> 20) & 0xFF)
    tstone = (tz16 > 0) & np.isin(tmatid, list(stone_ids))
    refhit = (rfl & 1).astype(bool)
    both = refhit & tstone

    dz = tz - rz                                    # + = bake is FURTHER than the reference
    adz = np.abs(dz[both])
    stone_n = int(refhit.sum())

    res = {
        "zscale": zscale,
        "stone_px_reference": stone_n,
        "stone_px_share": stone_n / float(w * h),
        "stone_matids": sorted(stone_ids),
        "both_px": int(both.sum()),
        "ref_only_px": int((refhit & ~tstone).sum()),   # definition has stone, bake does not
        "tess_only_px": int((~refhit & tstone).sum()),  # bake has stone, definition does not
        "dz_p50": pct(adz, 50), "dz_p90": pct(adz, 90), "dz_p99": pct(adz, 99),
        "dz_frac_gt_0.02": float((adz > 0.02).mean()) if adz.size else float("nan"),
        "dz_frac_gt_0.08": float((adz > 0.08).mean()) if adz.size else float("nan"),
        "dz_mean_signed": float(dz[both].mean()) if both.any() else float("nan"),
        "ref_step_px": int(((rfl & 4) != 0).sum()),
        "ref_skirt_px": int(((rfl & 8) != 0).sum()),
        "ref_grow_px": int(((rfl & 32) != 0).sum()),
        "ref_budget_px": int(((rfl & 16) != 0).sum()),
    }

    if tnrm is not None:
        d = np.clip((tnrm * rn).sum(-1), -1.0, 1.0)
        ang = np.degrees(np.arccos(d))[both]
        res["nrm_p50_deg"] = pct(ang, 50)
        res["nrm_p90_deg"] = pct(ang, 90)
        res["nrm_frac_gt_10deg"] = float((ang > 10.0).mean()) if ang.size else float("nan")
        res["nrm_frac_gt_30deg"] = float((ang > 30.0).mean()) if ang.size else float("nan")
    else:
        ang = None

    # ── BLOCK INTERIORS ────────────────────────────────────────────────────
    # design §6 P3's end condition is "dz p50 at block INTERIORS", i.e. on the
    # flat face of a block rather than on the shoulder the bake mip smears over
    # ~8 level-0 texels.  The reference renders n ∝ N − ∇ₛh, so a pixel is on a
    # flat part of the relief exactly when its normal equals its face's plane
    # normal — and the plane normal is recoverable from the reference alone as
    # the per-faceId MEDIAN normal, because the flat parts are the majority of
    # every face's area.  No camera, no amp=0 arm, no assumption about which
    # texels are mortar.
    flat = np.zeros((h, w), bool)
    crease = np.zeros((h, w), bool)
    if refhit.any():
        fids = rfid[refhit]
        order = np.argsort(fids, kind="stable")
        fs = fids[order]
        ns = rn[refhit][order]
        bounds = np.flatnonzero(np.diff(fs)) + 1
        planeN = {}
        for lo, hi in zip(np.r_[0, bounds], np.r_[bounds, fs.size]):
            if hi - lo < 200:
                continue
            m = np.median(ns[lo:hi], axis=0)
            n = np.linalg.norm(m)
            if n > 1e-9:
                planeN[int(fs[lo])] = m / n
        if planeN:
            ids = np.array(sorted(planeN))
            tbl = np.stack([planeN[i] for i in ids])
            idx = np.searchsorted(ids, rfid)
            idx = np.clip(idx, 0, ids.size - 1)
            known = refhit & (ids[idx] == rfid)
            pn = tbl[idx]
            cosang = np.clip((rn * pn).sum(-1), -1.0, 1.0)
            flat = known & (np.degrees(np.arccos(cosang)) < a.flat_deg)
            # ── the CREASE BAND (P4, §2e) ─────────────────────────────────
            # Two 4-neighbours on different faceIds whose PLANE normals differ
            # by more than the chart budget are looking across a real dihedral
            # junction; a same-plane seam (64.5% of all junction length,
            # 3eca6ffb3e40) is excluded on purpose, because the ring solve is
            # a no-op there by construction (both offset planes are one plane).
            if a.crease_band > 0:
                cosn = np.ones((h, w))
                bnd = np.zeros((h, w), bool)
                for ax, sh in ((0, 1), (0, -1), (1, 1), (1, -1)):
                    onp = np.roll(pn, sh, axis=ax)
                    ofid = np.roll(rfid, sh, axis=ax)
                    okn = np.roll(known, sh, axis=ax)
                    cosn = np.clip((pn * onp).sum(-1), -1.0, 1.0)
                    bnd |= (known & okn & (ofid != rfid) &
                            (np.degrees(np.arccos(cosn)) > a.crease_deg))
                band = bnd.copy()
                r = int(a.crease_band)
                for dy in range(-r, r + 1):
                    for dx in range(-r, r + 1):
                        if dx * dx + dy * dy > r * r:
                            continue
                        band |= np.roll(np.roll(bnd, dy, axis=0), dx, axis=1)
                crease = band & refhit
    bothflat = both & flat
    bothcrease = both & crease
    res["crease_band_px"] = int(a.crease_band)
    res["crease_deg"] = a.crease_deg
    res["crease_px"] = int(crease.sum())
    res["crease_share_of_stone"] = float(crease.sum()) / max(1, stone_n)
    res["dz_p50_crease"] = pct(np.abs(dz[bothcrease]), 50)
    res["dz_p90_crease"] = pct(np.abs(dz[bothcrease]), 90)
    res["flat_deg"] = a.flat_deg
    res["flat_px"] = int(flat.sum())
    res["flat_share_of_stone"] = float(flat.sum()) / max(1, stone_n)
    res["dz_p50_flat"] = pct(np.abs(dz[bothflat]), 50)
    res["dz_p90_flat"] = pct(np.abs(dz[bothflat]), 90)

    if a.bare:
        bz16, bmat, bnrm = load_arm(a.bare, w, h)
        bz = np.where(bz16 > 0, (65408.0 - bz16.astype(np.float64)) / zscale, 0.0)
        mb = refhit & (bz16 > 0) & np.isin((bmat >> 20) & 0xFF, list(stone_ids))
        res["bare_dz_p50"] = pct(np.abs((bz - rz)[mb]), 50)
        res["bare_dz_p90"] = pct(np.abs((bz - rz)[mb]), 90)
        res["bare_dz_p50_flat"] = pct(np.abs((bz - rz)[mb & flat]), 50)
        res["bare_dz_p90_flat"] = pct(np.abs((bz - rz)[mb & flat]), 90)
        # the phase's own gate: the arm's block-interior p50 against twice the
        # bare wall's, both measured on the same mask through the same reference
        if res["bare_dz_p50_flat"] == res["bare_dz_p50_flat"]:
            res["p3_gate_ratio"] = res["dz_p50_flat"] / max(1e-9, res["bare_dz_p50_flat"])
            res["p3_gate_pass"] = bool(res["p3_gate_ratio"] <= 2.0)

    if a.out_prefix:
        for _d in (a.tess, a.ref, a.bare):
            if not _d:
                continue
            _p = arm_provenance_source(_d)
            if _p:
                _PROV_INPUTS.append(_p)
        # signed dz, diverging, +-0.15 u
        img = np.zeros((h, w, 3), np.uint8)
        img[both] = colormap(dz, -0.15, 0.15, "diverging")[both]
        save_png(a.out_prefix + "_dz.png", img)
        # normal angle 0..45 deg
        if ang is not None:
            am = np.zeros((h, w), np.float64)
            am[both] = np.degrees(np.arccos(np.clip((tnrm * rn).sum(-1), -1, 1)))[both]
            img = np.zeros((h, w, 3), np.uint8)
            img[both] = colormap(am, 0.0, 45.0)[both]
            save_png(a.out_prefix + "_nrm.png", img)
        # LAPLACIAN-WEIGHTED |difference| (Shafiee Sarvestani et al. 2024):
        # |dz| alone is dominated by smooth global offset; weighting by the local
        # Laplacian of the reference leaves the structural disagreement.
        lap = np.abs(laplacian(np.where(refhit, rz, 0.0)))
        lw = np.abs(dz) * (lap / max(1e-9, float(np.percentile(lap[refhit], 95)) if refhit.any() else 1.0))
        img = np.zeros((h, w, 3), np.uint8)
        img[both] = colormap(lw, 0.0, 0.05)[both]
        save_png(a.out_prefix + "_lapw.png", img)
        # crack map: red = reference has surface, bake has none; blue = the reverse
        img = np.zeros((h, w, 3), np.uint8)
        img[refhit & ~tstone] = (255, 40, 40)
        img[~refhit & tstone] = (40, 90, 255)
        save_png(a.out_prefix + "_crack.png", img)
        res["maps"] = [a.out_prefix + s for s in ("_dz.png", "_nrm.png", "_lapw.png", "_crack.png")]

    if a.json:
        print(json.dumps(res, indent=1))
    else:
        for k, v in res.items():
            print("%-22s %s" % (k, ("%.4f" % v) if isinstance(v, float) else v))


if __name__ == "__main__":
    main()
