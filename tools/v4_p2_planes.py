#!/usr/bin/env python3
"""Two-tier byte-identity comparison for the v4 phase-2 undisplaced lattice.

    tools/v4_p2_planes.py <dirA> <dirB> [--label X]

`dirA` is the CONTROL arm (--v4_flat: the authored stone triangles through the
v4 pipeline) and `dirB` the LATTICE arm.  Each directory holds one greets
snapshot triple written by tools/v4_p2_pose.sh:

    greets_tNNNNNN_color.ppm     the shipped image
    greets_tNNNNNN_depth.z16     ZPage16, word per pixel   (FDS_SNAPSHOT_ZDUMP)
    greets_tNNNNNN_mat.u32       G-buffer mip:4|matID:8|uv:20 (FDS_SNAPSHOT_GBUFDUMP)

plus a `log.txt` carrying the [GBUFDUMP] id=N name=... table.

TIER 1 is byte-identity on all three planes.  When that fails, TIER 2 is the
weakened gate the coordinator specified, and it reports every number it needs:

  * |z16 delta| histogram, and whether it exceeds ONE quantum anywhere;
  * raster-vs-empty flips (z16 == 0 on exactly one side — background showing
    through, or new coverage);
  * matID flips, split by whether either side is a STONE material.  This is the
    one that matters: the tree has just learned (platform.m5.missing_polys) that
    a co-planar +-1 LSB flip can hand a pixel to a different SHEET, and the
    ceiling water overlay is the sheet that shows it;
  * colour: differing pixels, max per-channel delta, and the differing pixels'
    distribution over the matID plane.

Exit 0 on tier 1, 1 on tier 2 (report only), 2 on a hard failure (a tier-2
criterion violated) or a missing input.
"""
import os
import re
import sys
import struct
import collections

import numpy as np

# The ::mirUV clones ARE the stone: GreetsMirror splits every stone face into a
# mirror-UV clone after the bake, and the main view rasterises the clones.
STONE = {"rooms", "floor", "rooms::mirUV", "floor::mirUV"}


def find(d, suffix):
    for f in sorted(os.listdir(d)):
        if f.endswith(suffix):
            return os.path.join(d, f)
    return None


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a P6 PPM")
    # header: P6 <w> <h> <max>\n
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(data) and data[idx : idx + 1].isspace():
            idx += 1
        if data[idx : idx + 1] == b"#":
            while data[idx : idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx : idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx : idx + w * h * 3]


def matnames(logpath):
    names = {}
    if not os.path.exists(logpath):
        return names
    with open(logpath, "r", errors="replace") as f:
        for line in f:
            m = re.match(r"\[GBUFDUMP\] id=(\d+) name=(.*)$", line.strip())
            if m:
                names[int(m.group(1))] = m.group(2)
    return names


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    A, B = sys.argv[1], sys.argv[2]
    label = "pose"
    if "--label" in sys.argv:
        label = sys.argv[sys.argv.index("--label") + 1]

    ca, cb = find(A, "_color.ppm"), find(B, "_color.ppm")
    za, zb = find(A, "_depth.z16"), find(B, "_depth.z16")
    ma, mb = find(A, "_mat.u32"), find(B, "_mat.u32")
    for p in (ca, cb, za, zb, ma, mb):
        if not p:
            print(f"{label}: MISSING plane in {A} / {B}")
            sys.exit(2)

    w, h, pa = read_ppm(ca)
    w2, h2, pb = read_ppm(cb)
    if (w, h) != (w2, h2):
        print(f"{label}: size mismatch {w}x{h} vs {w2}x{h2}")
        sys.exit(2)
    n = w * h
    Za = np.frombuffer(open(za, "rb").read()[: n * 2], dtype="<u2")
    Zb = np.frombuffer(open(zb, "rb").read()[: n * 2], dtype="<u2")
    Ma = np.frombuffer(open(ma, "rb").read()[: n * 4], dtype="<u4")
    Mb = np.frombuffer(open(mb, "rb").read()[: n * 4], dtype="<u4")
    names = matnames(os.path.join(A, "log.txt")) or matnames(os.path.join(B, "log.txt"))

    Pa = np.frombuffer(pa, dtype=np.uint8).reshape(n, 3)
    Pb = np.frombuffer(pb, dtype=np.uint8).reshape(n, 3)
    same_color = bool(np.array_equal(Pa, Pb))
    same_z = bool(np.array_equal(Za, Zb))
    same_m = bool(np.array_equal(Ma, Mb))
    if same_color and same_z and same_m:
        print(f"{label}: TIER1 byte-identical (color+z16+mat, {w}x{h})")
        sys.exit(0)

    # ── tier 2 ────────────────────────────────────────────────────────────
    dz = np.abs(Za.astype(np.int64) - Zb.astype(np.int64))
    nzd = dz[dz != 0]
    zmax = int(nzd.max()) if nzd.size else 0
    vals, cnts = np.unique(nzd, return_counts=True)
    zhist = {int(v): int(c) for v, c in zip(vals[:6], cnts[:6])}
    over = np.nonzero(dz > 1)[0]
    empty_flip = int(np.count_nonzero((Za == 0) != (Zb == 0)))
    flip_idx = np.nonzero((Za == 0) != (Zb == 0))[0]

    SENT = (Ma == 0xFFFFFFFF) | (Ma == 0xFFFFFFFE)
    SENTB = (Mb == 0xFFFFFFFF) | (Mb == 0xFFFFFFFE)
    ida = np.where(SENT, 255, (Ma >> 20) & 0xFF).astype(np.int32)
    idb = np.where(SENTB, 255, (Mb >> 20) & 0xFF).astype(np.int32)
    ida[SENT] = -1
    idb[SENTB] = -1
    mflip = ida != idb
    mat_flip = int(np.count_nonzero(mflip))
    flip_pairs = collections.Counter()
    stone_ids = {i for i, nm in names.items() if nm in STONE}
    mat_flip_nonstone = 0
    for i in np.nonzero(mflip)[0]:
        na = names.get(int(ida[i]), f"id{int(ida[i])}")
        nb = names.get(int(idb[i]), f"id{int(idb[i])}")
        flip_pairs[(na, nb)] += 1
        if na not in STONE or nb not in STONE:
            mat_flip_nonstone += 1

    cd = np.abs(Pa.astype(np.int16) - Pb.astype(np.int16)).max(axis=1)
    cdiff = int(np.count_nonzero(cd))
    cmax = int(cd.max()) if n else 0
    mipd = int(np.count_nonzero(((Ma >> 28) != (Mb >> 28)) & ~SENT & ~SENTB))

    print(f"{label}: TIER2 (not byte-identical), {w}x{h} = {n} px")
    print(f"{label}:   z16   differing={int(nzd.size)} max|delta|={zmax} hist={zhist}")
    print(f"{label}:   z16   over_1_quantum={int(over.size)} at "
          f"{[(int(i%w), int(i//w)) for i in over[:6]]}")
    print(f"{label}:   z16   raster_vs_empty_flips={empty_flip} at "
          f"{[(int(i%w), int(i//w)) for i in flip_idx[:6]]}")
    print(f"{label}:   mat   matID_flips={mat_flip} involving_non_stone={mat_flip_nonstone} "
          f"mip_flips={mipd}")
    for (na, nb), c in flip_pairs.most_common(8):
        print(f"{label}:         {na} -> {nb}: {c}")
    print(f"{label}:   color differing={cdiff} ({100.0*cdiff/n:.4f}%) max_channel_delta={cmax}")
    order = np.argsort(-np.bincount(np.maximum(ida, 0), weights=(cd != 0).astype(np.float64),
                                    minlength=256))
    tot = np.bincount(np.maximum(ida, 0), minlength=256)
    hitc = np.bincount(np.maximum(ida, 0), weights=(cd != 0).astype(np.float64), minlength=256)
    for k in order[:8]:
        if hitc[k] == 0:
            break
        nm = names.get(int(k), f"id{int(k)}")
        print(f"{label}:         {nm}: {int(hitc[k])} of {int(tot[k])} "
              f"({100.0*hitc[k]/max(1,tot[k]):.1f}% of that sheet)")

    bad = []
    if zmax > 1:
        bad.append(f"|z16 delta| {zmax} > 1 quantum")
    if empty_flip:
        bad.append(f"{empty_flip} raster-vs-empty flips")
    if mat_flip_nonstone:
        bad.append(f"{mat_flip_nonstone} matID flips involving a non-stone sheet")
    if bad:
        print(f"{label}: TIER2 FAIL — " + "; ".join(bad))
        sys.exit(2)
    print(f"{label}: TIER2 PASS — |z16| <= 1, 0 coverage flips, 0 non-stone ownership flips")
    sys.exit(1)


if __name__ == "__main__":
    main()
