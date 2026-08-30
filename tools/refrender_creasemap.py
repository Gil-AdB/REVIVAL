#!/usr/bin/env python3
"""Crease-dh map: where on screen the two sides of a junction disagree, and by how much.

--greets_displace_ref_crease_viz=N writes, for every hit pixel within N texels of
a shared stone crease, the SIGNED disagreement d_self - d_neighbour in world
units at that exact world point (REFRND02's extra plane; NaN elsewhere).  The
crease census (--greets_displace_ref_crease_scan) says WHICH junctions disagree
and whether a shift reconciles them; this says where the eye meets one.

Writes a diverging map (blue = the neighbour stands proud, red = this side does)
over a dimmed copy of the render, and prints the distribution.

usage:
  tools/refrender_creasemap.py DIR [--range 0.08] [--out PATH] [--json]
     DIR holds ref.bin and the pose's *_color.png / *_color.ppm
"""
import argparse, glob, json, os, sys
import numpy as np


def load(path):
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != b"REFRND02":
            sys.exit("%s is %s; re-render with --greets_displace_ref_crease_viz=N "
                     "to get the crease plane" % (path, magic.decode("ascii", "replace")))
        w, h = (int(x) for x in np.frombuffer(f.read(8), dtype=np.int32))
        n = w * h
        f.read(n * 4); f.read(n * 12); f.read(n * 4)
        fl = np.frombuffer(f.read(n * 4), dtype=np.uint32).reshape(h, w)
        ch = np.frombuffer(f.read(n * 4), dtype=np.float32).reshape(h, w).astype(np.float64)
    return w, h, fl, ch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--range", type=float, default=0.08,
                    help="world units at full saturation (default 0.08; the height "
                         "field's own |d|max for greets stone is 0.164)")
    ap.add_argument("--out")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    from PIL import Image
    w, h, fl, ch = load(os.path.join(a.dir, "ref.bin"))
    hit = (fl & 1) != 0
    band = np.isfinite(ch) & hit

    col = None
    for pat in ("*_color.png", "*_color.ppm"):
        g = sorted(glob.glob(os.path.join(a.dir, pat)))
        if g:
            col = np.array(Image.open(g[0]).convert("RGB")).astype(np.float64)
            break
    img = (col * 0.42).astype(np.uint8) if col is not None else np.zeros((h, w, 3), np.uint8)

    t = np.clip(np.nan_to_num(ch, nan=0.0) / max(1e-9, a.range), -1.0, 1.0)
    r = np.clip(t, 0, 1)
    b = np.clip(-t, 0, 1)
    g_ = 1.0 - np.abs(t)
    rgb = (np.stack([r, g_ * 0.55, b], -1) * 255.0 + 0.5).astype(np.uint8)
    img[band] = rgb[band]

    out = a.out or os.path.join(a.dir, "creasemap.png")
    Image.fromarray(img, "RGB").save(out)

    v = ch[band]
    av = np.abs(v)
    res = {"png": out, "band_px": int(band.sum()), "hit_px": int(hit.sum()),
           "band_share": float(band.sum()) / max(1, int(hit.sum())),
           "dh_p50": float(np.percentile(av, 50)) if av.size else 0.0,
           "dh_p90": float(np.percentile(av, 90)) if av.size else 0.0,
           "dh_p99": float(np.percentile(av, 99)) if av.size else 0.0,
           "dh_max": float(av.max()) if av.size else 0.0,
           "dh_mean_signed": float(v.mean()) if v.size else 0.0,
           "px_over_0.05u": int((av > 0.05).sum()), "px_over_0.10u": int((av > 0.10).sum())}
    print(json.dumps(res, indent=1) if a.json else
          "\n".join("%-16s %s" % (k, ("%.5f" % x) if isinstance(x, float) else x)
                    for k, x in res.items()))


if __name__ == "__main__":
    main()
