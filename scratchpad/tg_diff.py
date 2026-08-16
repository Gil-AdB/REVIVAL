#!/usr/bin/env python3
"""tg_diff.py A.ppm B.ppm [--grid WxH] [--res WxH] [--png OUT.png]

Judge-call diff report for the frame raster tile-grid change: how many pixels
moved, by how much, and — the question the grid change actually raises — WHERE.

"Seam-local" is not eyeballed here: given the two grids the change moves between,
the script computes both grids' INTERIOR tile boundaries (the same
`tileSize = ceil(res/n) & ~7` rule renderFrame uses) and reports the share of
changed pixels within a band of each. A change that is genuinely a rasterization
seam artefact concentrates there; a change that is global does not.
"""
import sys, zlib, struct

def read_ppm(p):
    d = open(p, 'rb').read()
    i = 0; f = []
    while len(f) < 4:
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        f.append(d[i:j]); i = j
    i += 1
    w, h = int(f[1]), int(f[2])
    return w, h, d[i:i + w*h*3]

def tile_bounds(res, n):
    """Interior boundary coordinates of renderFrame's tiler for `n` tiles."""
    raw = (res + n - 1) // n
    ts = raw & ~7
    if ts <= 0: return []
    return [ts * k for k in range(1, n) if ts * k < res]

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)

def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    grids, png = [], None
    it = iter(range(len(args)))
    k = 0
    while k < len(args):
        if args[k] == '--grid': grids.append(args[k+1]); k += 2
        elif args[k] == '--png': png = args[k+1]; k += 2
        else: k += 1

    w, h, A = read_ppm(a_path)
    w2, h2, B = read_ppm(b_path)
    assert (w, h) == (w2, h2), "size mismatch %dx%d vs %dx%d" % (w, h, w2, h2)
    n = w * h

    try:
        import numpy as np
    except ImportError:
        np = None

    if np is not None:
        a = np.frombuffer(A, dtype=np.uint8)[:n*3].reshape(h, w, 3).astype(np.int16)
        b = np.frombuffer(B, dtype=np.uint8)[:n*3].reshape(h, w, 3).astype(np.int16)
        d = np.abs(a - b).max(axis=2)
        mask = d > 0
        cnt = int(mask.sum()); maxd = int(d.max()) if cnt else 0
        mean_over_changed = float(d[mask].mean()) if cnt else 0.0
        vals, counts = np.unique(d[mask], return_counts=True) if cnt else ([], [])
        hist = list(zip([int(v) for v in vals], [int(c) for c in counts]))
    else:
        raise SystemExit("numpy required")

    print("%s vs %s   %dx%d = %d px" % (a_path.split('/')[-1], b_path.split('/')[-1], w, h, n))
    print("  changed : %d px (%.4f %%)" % (cnt, 100.0 * cnt / n))
    if not cnt:
        return
    print("  max |d| : %d/255      mean |d| over changed px: %.2f" % (maxd, mean_over_changed))
    print("  hist    : " + ", ".join("|d|=%d:%d" % (v, c) for v, c in hist[:10])
          + (" ..." if len(hist) > 10 else ""))

    # ── WHERE: seam locality against each named grid's interior boundaries ──
    ys, xs = np.nonzero(mask)
    for g in grids:
        gx, gy = (int(t) for t in g.split('x'))
        bx, by = tile_bounds(w, gx), tile_bounds(h, gy)
        for band in (1, 8, 32):
            near = np.zeros(cnt, dtype=bool)
            for c in bx: near |= np.abs(xs - c) < band
            for c in by: near |= np.abs(ys - c) < band
            # Area share of the band, so "concentrated" is measured against
            # what a UNIFORM scatter would put there.
            colmask = np.zeros(w, dtype=bool)
            rowmask = np.zeros(h, dtype=bool)
            for c in bx: colmask[max(0, c-band+1):c+band] = True
            for c in by: rowmask[max(0, c-band+1):c+band] = True
            area = (colmask.sum() * h + rowmask.sum() * w
                    - colmask.sum() * rowmask.sum())
            print("    grid %-6s band +-%2d px: %6.2f %% of changed px  "
                  "(band is %5.2f %% of the frame -> %.1fx enrichment)"
                  % (g, band, 100.0 * near.sum() / cnt, 100.0 * area / n,
                     (near.sum() / cnt) / (area / n) if area else 0.0))

    print("  bbox    : x %d..%d  y %d..%d" % (xs.min(), xs.max(), ys.min(), ys.max()))
    # Row/column concentration: the top 5 rows and columns by changed-px count.
    rowc = np.bincount(ys, minlength=h); colc = np.bincount(xs, minlength=w)
    print("  top rows: " + ", ".join("y=%d:%d" % (i, rowc[i]) for i in np.argsort(rowc)[-5:][::-1]))
    print("  top cols: " + ", ".join("x=%d:%d" % (i, colc[i]) for i in np.argsort(colc)[-5:][::-1]))

    if png:
        # Amplified diff: changed px in hot colour scaled by |d|, unchanged dark.
        out = np.zeros((h, w, 3), dtype=np.uint8)
        amp = np.clip(d.astype(np.int32) * 24, 0, 255).astype(np.uint8)
        out[..., 0] = amp
        out[..., 1] = np.clip(d.astype(np.int32) * 6, 0, 255).astype(np.uint8)
        out[..., 2] = (mask * 40).astype(np.uint8)
        write_png(png, w, h, out.tobytes())
        print("  wrote   : %s" % png)

main()
