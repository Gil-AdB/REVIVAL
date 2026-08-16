#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png [--maxw N] — PPM -> PNG, optional box downscale.

Zero dependencies beyond numpy+zlib so it runs anywhere the bench does.
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
    return w, h, np.frombuffer(d[i:i + w*h*3], dtype=np.uint8).reshape(h, w, 3)


def write_png(path, arr):
    h, w, _ = arr.shape
    raw = b''.join(b'\x00' + arr[y].tobytes() for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 6))
        + chunk(b'IEND', b''))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    maxw = 0
    if "--maxw" in sys.argv:
        maxw = int(sys.argv[sys.argv.index("--maxw") + 1])
    w, h, a = read_ppm(src)
    if maxw and w > maxw:
        k = (w + maxw - 1) // maxw
        hh, ww = (h // k) * k, (w // k) * k
        a = a[:hh, :ww].reshape(hh // k, k, ww // k, k, 3).mean(axis=(1, 3)).astype(np.uint8)
    write_png(dst, np.ascontiguousarray(a))
    print("%s -> %s (%dx%d)" % (src, dst, a.shape[1], a.shape[0]))


main()
