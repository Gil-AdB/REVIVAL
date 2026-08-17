#!/usr/bin/env python3
"""reflmir_img.py — before/after image products for the --refl_correct round.

  reflmir_img.py where   BEFORE.ppm AFTER.ppm OUT.png [--maxw N]
  reflmir_img.py sbs     BEFORE.ppm AFTER.ppm OUT.png x0 y0 w h [--maxw N]
  reflmir_img.py full    IN.ppm OUT.png [--maxw N]
  reflmir_img.py bbox    BEFORE.ppm AFTER.ppm          # print changed-pixel bbox
"""
import sys, zlib, struct
import numpy as np


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
    return np.frombuffer(d[i:i + w*h*3], dtype=np.uint8).reshape(h, w, 3)


def write_png(path, arr):
    h, w, _ = arr.shape
    raw = b''.join(b'\x00' + arr[y].tobytes() for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def box_down(a, maxw):
    h, w, _ = a.shape
    if maxw <= 0 or w <= maxw:
        return a
    k = int(np.ceil(w / maxw))
    hh, ww = (h // k) * k, (w // k) * k
    a = a[:hh, :ww].astype(np.uint16)
    a = a.reshape(hh // k, k, ww // k, k, 3).mean(axis=(1, 3))
    return a.astype(np.uint8)


def argmaxw(args):
    if '--maxw' in args:
        i = args.index('--maxw')
        return int(args[i+1])
    return 0


def main():
    mode = sys.argv[1]
    mw = argmaxw(sys.argv)
    if mode == 'full':
        a = read_ppm(sys.argv[2])
        write_png(sys.argv[3], box_down(a, mw))
    elif mode == 'where':
        A = read_ppm(sys.argv[2]); B = read_ppm(sys.argv[3])
        d = (np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2) > 0)
        out = (A.astype(np.uint16) // 2).astype(np.uint8)
        out[d] = [255, 0, 255]
        write_png(sys.argv[4], box_down(out, mw))
    elif mode == 'sbs':
        A = read_ppm(sys.argv[2]); B = read_ppm(sys.argv[3])
        x0, y0, w, h = (int(v) for v in sys.argv[5:9])
        ca, cb = A[y0:y0+h, x0:x0+w], B[y0:y0+h, x0:x0+w]
        gap = np.full((h, 8, 3), 255, np.uint8)
        write_png(sys.argv[4], box_down(np.concatenate([ca, gap, cb], axis=1), mw))
    elif mode == 'tri':
        A = read_ppm(sys.argv[2]); B = read_ppm(sys.argv[3]); C = read_ppm(sys.argv[4])
        x0, y0, w, h = (int(v) for v in sys.argv[6:10])
        gap = np.full((h, 8, 3), 255, np.uint8)
        cs = [X[y0:y0+h, x0:x0+w] for X in (A, B, C)]
        write_png(sys.argv[5], box_down(np.concatenate([cs[0], gap, cs[1], gap, cs[2]], axis=1), mw))
    elif mode == 'bbox':
        A = read_ppm(sys.argv[2]); B = read_ppm(sys.argv[3])
        d = (np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2) > 0)
        if not d.any():
            print('no change'); return
        ys, xs = np.where(d)
        print('changed bbox x=[%d..%d] y=[%d..%d]  n=%d  (frame %dx%d)'
              % (xs.min(), xs.max(), ys.min(), ys.max(), d.sum(), A.shape[1], A.shape[0]))
        # row profile: where in Y the change concentrates (deciles)
        rows = d.sum(axis=1)
        for k in range(10):
            lo, hi = k * A.shape[0] // 10, (k+1) * A.shape[0] // 10
            print('   y %4d-%4d : %8d' % (lo, hi, rows[lo:hi].sum()))


main()
