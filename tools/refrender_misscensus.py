#!/usr/bin/env python3
"""Spatial census of the reference renderer's MISS pixels.

A "miss" is a pixel the rasteriser painted stone and the reference ray found no
surface on at all (FDS/RENDER/DeferredDisplaceRef.cpp, pxFallback; flags bit 1).
The engine's own [REFRENDER-MISS] banner partitions those pixels by CAUSE.  This
tool answers the other half -- their SHAPE -- because the two readings are not
the same defect:

  * a one-pixel RIM along a silhouette is the float rasteriser and the double
    reference disagreeing about where a triangle ends, and is not a hole;
  * a compact BLOB inside a wall is a hole in the model, and is.

It reads only ref.bin (the reference's own dump: z, normal, faceId, flags), so
it needs no second arm and no snapshot planes.

  flags bit 0  hit          bit 5   grew past the rasteriser
        bit 1  MISS         bit 6   an event was dropped   (H1)
        bit 2  bisector step bit 7  a SLAB event was dropped (H1)
        bit 3  free-edge skirt bit 8 candidate cap hit
        bit 4  march budget  bits 12-14  miss cause 0..7
        bits 16-31  dropped-event count

usage: tools/refrender_misscensus.py REF.BIN [--png OUT.png] [--json]
"""
import argparse, json, os, sys
import numpy as np

CAUSE = ["empty-bin", "all-backfacing", "all-clipped", "never-inside",
         "margin-arrival-discard", "no-boundary-kind", "before-near", "other"]
# red, green, blue per cause (only 3..5 occur in practice)
CAUSE_RGB = [(255,255,0), (255,0,255), (0,255,255), (255,60,60),
             (60,120,255), (60,255,120), (255,160,0), (200,200,200)]


def load(path):
    with open(path, "rb") as f:
        assert f.read(8) == b"REFRND01", "not a REFRND01 dump: " + path
        w, h = (int(x) for x in np.frombuffer(f.read(8), dtype=np.int32))
        n = w * h
        z = np.frombuffer(f.read(n * 4), dtype=np.float32).reshape(h, w)
        f.read(n * 12)                                   # normals, unused here
        fid = np.frombuffer(f.read(n * 4), dtype=np.int32).reshape(h, w)
        fl = np.frombuffer(f.read(n * 4), dtype=np.uint32).reshape(h, w)
    return w, h, z, fid, fl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("refbin")
    ap.add_argument("--png", help="write a cause-coloured miss map here")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    w, h, z, fid, fl = load(a.refbin)
    hit = (fl & 1) != 0
    miss = (fl & 2) != 0
    cause = ((fl >> 12) & 7)

    from scipy import ndimage
    lab, nlab = ndimage.label(miss, structure=np.ones((3, 3), np.int32))
    sizes = np.bincount(lab.ravel())[1:] if nlab else np.array([], np.int64)

    # RIM vs INTERIOR.  A miss pixel is a rim pixel when it touches a pixel that
    # is neither a hit nor a miss -- i.e. the reference's stone ends there, which
    # is exactly where a silhouette disagreement lives.  An interior miss is
    # surrounded by surface on every side.
    stone = hit | miss
    pad = np.pad(stone, 1, constant_values=False)
    nbr_all = (pad[:-2,1:-1] & pad[2:,1:-1] & pad[1:-1,:-2] & pad[1:-1,2:] &
               pad[:-2,:-2] & pad[:-2,2:] & pad[2:,:-2] & pad[2:,2:])
    rim = miss & ~nbr_all
    interior = miss & nbr_all

    res = {
        "res": [w, h],
        "miss_px": int(miss.sum()),
        "hit_px": int(hit.sum()),
        "components": int(nlab),
        "rim_px": int(rim.sum()),
        "interior_px": int(interior.sum()),
        "rim_share": float(rim.sum()) / max(1, int(miss.sum())),
        "comp_size_max": int(sizes.max()) if sizes.size else 0,
        "comp_size_p50": float(np.percentile(sizes, 50)) if sizes.size else 0.0,
        "comp_size_p90": float(np.percentile(sizes, 90)) if sizes.size else 0.0,
        "comp_size_mean": float(sizes.mean()) if sizes.size else 0.0,
        "comp_hist": {  # how the MISS PIXELS distribute over component size
            "1px":      int(sizes[sizes == 1].sum()),
            "2-4px":    int(sizes[(sizes >= 2) & (sizes <= 4)].sum()),
            "5-16px":   int(sizes[(sizes >= 5) & (sizes <= 16)].sum()),
            "17-64px":  int(sizes[(sizes >= 17) & (sizes <= 64)].sum()),
            "65-256px": int(sizes[(sizes >= 65) & (sizes <= 256)].sum()),
            ">256px":   int(sizes[sizes > 256].sum()),
        },
        "cause_px": {CAUSE[c]: int((miss & (cause == c)).sum()) for c in range(8)
                     if (miss & (cause == c)).any()},
        "cause_interior_px": {CAUSE[c]: int((interior & (cause == c)).sum()) for c in range(8)
                              if (interior & (cause == c)).any()},
        "faces_touched": int(np.unique(fid[miss]).size) if miss.any() else 0,
    }
    # the ten biggest holes, with their bounding boxes and dominant cause
    if nlab:
        order = np.argsort(sizes)[::-1][:10]
        objs = ndimage.find_objects(lab)
        big = []
        for i in order:
            sl = objs[i]
            m = (lab[sl] == i + 1)
            cs = cause[sl][m]
            dom = int(np.bincount(cs, minlength=8).argmax())
            big.append({"size": int(sizes[i]),
                        "bbox_xywh": [int(sl[1].start), int(sl[0].start),
                                      int(sl[1].stop - sl[1].start),
                                      int(sl[0].stop - sl[0].start)],
                        "cause": CAUSE[dom],
                        "faces": sorted(set(int(x) for x in np.unique(fid[sl][m])))[:6]})
        res["biggest"] = big

    if a.png:
        from PIL import Image
        img = np.zeros((h, w, 3), np.uint8)
        img[hit] = (38, 38, 38)
        for c in range(8):
            m = miss & (cause == c)
            if m.any():
                img[m] = CAUSE_RGB[c]
        Image.fromarray(img, "RGB").save(a.png)
        res["png"] = a.png

    if a.json:
        print(json.dumps(res, indent=1))
    else:
        for k, v in res.items():
            print("%-20s %s" % (k, v if not isinstance(v, float) else "%.4f" % v))


if __name__ == "__main__":
    main()
