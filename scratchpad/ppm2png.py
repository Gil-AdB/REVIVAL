#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png [--maxw N] — PPM -> PNG, optional box downscale.

Zero dependencies beyond numpy+zlib so it runs anywhere the bench does.

SUPERSEDED BY tools/ppm2png.py, which does the same job (including --maxw) and
is the canonical converter. This copy is kept because ~150 scratchpad batteries
call it by path; it now embeds the same `groundwork-provenance` chunk so
anything it has already produced is not the odd one out.
"""
import os, sys, zlib, struct
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "tools"))
from provenance import KEYWORD, read_sidecar   # noqa: E402
import json                                    # noqa: E402


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


def write_png(path, arr, text=None):
    h, w, _ = arr.shape
    raw = b''.join(b'\x00' + arr[y].tobytes() for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    blob = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
    if text is not None:
        blob += chunk(b'tEXt', KEYWORD.encode('latin-1') + b'\x00'
                      + text.encode('latin-1', 'replace'))
    blob += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(blob)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    maxw = 0
    if "--maxw" in sys.argv:
        maxw = int(sys.argv[sys.argv.index("--maxw") + 1])
    w, h, a = read_ppm(src)
    down = None
    if maxw and w > maxw:
        k = (w + maxw - 1) // maxw
        hh, ww = (h // k) * k, (w // k) * k
        a = a[:hh, :ww].reshape(hh // k, k, ww // k, k, 3).mean(axis=(1, 3)).astype(np.uint8)
        down = {"factor": k, "from": [w, h], "to": [ww // k, hh // k]}
    sidecar = read_sidecar(src)
    text = None
    if sidecar is not None:
        rec = dict(sidecar)
        rec["converted_by"] = {"tool": "scratchpad/ppm2png.py",
                               "source": os.path.basename(src)}
        if down:
            rec["converted_by"]["downscale"] = down
        text = json.dumps(rec, ensure_ascii=True)
    write_png(dst, np.ascontiguousarray(a), text)
    print("%s -> %s (%dx%d)%s" % (src, dst, a.shape[1], a.shape[0],
                                  "" if text else "  [no provenance sidecar]"))


main()
