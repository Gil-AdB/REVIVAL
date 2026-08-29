#!/usr/bin/env python3
"""Convert an engine PPM/PGM to PNG, carrying its provenance sidecar into the file.

Usage:
    tools/ppm2png.py <in.ppm> [out.png] [--maxw N]
    tools/ppm2png.py <in.ppm> --print         convert nothing, print the sidecar

--maxw N box-downscales by an integer factor so the long edge is <= N (the
report-sized copy). The record then carries a `downscale` note, because a
downscaled render is not the render the pixel numbers were measured on.

The engine writes `<stem>.json` beside every PPM (FDS/Base/Provenance.cpp).
That JSON is embedded here as a PNG `tEXt` chunk keyed `groundwork-provenance`,
so the PNG still answers "which binary, which flags, which camera, which t?"
after it has been copied into docs/img/ and linked into a report — which is the
point at which the sidecar would otherwise have been left behind.

Read it back with tools/png_provenance.py.

No PIL: the encoder here is stdlib zlib + struct. That keeps the tool usable
from any checkout and, more to the point, makes the byte layout of the chunk
ours rather than an encoder's. numpy is imported lazily and only for --maxw.
"""

from __future__ import annotations

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from provenance import KEYWORD, PNG_MAGIC, read_sidecar  # noqa: E402
import json  # noqa: E402
import binascii  # noqa: E402


def _read_netpbm(path: str):
    """Minimal P5/P6 reader. Returns (width, height, maxval, kind, pixels)."""
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:2] not in (b"P5", b"P6"):
        raise SystemExit(f"{path}: not a binary PGM (P5) or PPM (P6)")
    kind = blob[:2].decode()
    # Header tokens are whitespace separated; '#' starts a comment to EOL.
    pos, tokens = 2, []
    while len(tokens) < 3:
        while pos < len(blob) and blob[pos:pos + 1].isspace():
            pos += 1
        if blob[pos:pos + 1] == b"#":
            while pos < len(blob) and blob[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(blob) and not blob[pos:pos + 1].isspace():
            pos += 1
        tokens.append(blob[start:pos])
    pos += 1                       # exactly one whitespace byte after maxval
    w, h, maxval = (int(t) for t in tokens)
    return w, h, maxval, kind, blob[pos:]


def _chunk(ctype: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + ctype + data
            + struct.pack(">I", binascii.crc32(ctype + data) & 0xFFFFFFFF))


def _write_png(path: str, w: int, h: int, bitdepth: int, colortype: int,
               raw_rows: bytes, bytes_per_px: int, text: str | None) -> None:
    stride = w * bytes_per_px
    # Filter type 0 (None) on every row: these are diagnostic renders, and a
    # byte-predictable encoding is worth more here than a few percent of size.
    scan = bytearray()
    for y in range(h):
        scan += b"\x00"
        scan += raw_rows[y * stride:(y + 1) * stride]
    out = bytearray(PNG_MAGIC)
    out += _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bitdepth, colortype, 0, 0, 0))
    if text is not None:
        out += _chunk(b"tEXt", KEYWORD.encode("latin-1") + b"\x00"
                      + text.encode("latin-1", "replace"))
    out += _chunk(b"IDAT", zlib.compress(bytes(scan), 6))
    out += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(bytes(out))


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    src = argv[1]
    sidecar = read_sidecar(src)

    if len(argv) > 2 and argv[2] == "--print":
        if sidecar is None:
            print(f"{src}: no provenance sidecar", file=sys.stderr)
            return 1
        print(json.dumps(sidecar, indent=2))
        return 0

    rest = [a for a in argv[2:] if not a.startswith("--")]
    maxw = 0
    if "--maxw" in argv:
        maxw = int(argv[argv.index("--maxw") + 1])
        if rest and rest[-1] == str(maxw):
            rest = rest[:-1]
    dst = rest[0] if rest else os.path.splitext(src)[0] + ".png"
    w, h, maxval, kind, pixels = _read_netpbm(src)

    if kind == "P6":
        bpp, colortype = (3, 2) if maxval < 256 else (6, 2)
        bitdepth = 8 if maxval < 256 else 16
    else:
        bpp, colortype = (1, 0) if maxval < 256 else (2, 0)
        bitdepth = 8 if maxval < 256 else 16
    need = w * h * bpp
    if len(pixels) < need:
        raise SystemExit(f"{src}: truncated ({len(pixels)} of {need} pixel bytes)")

    down = None
    if maxw and w > maxw and bitdepth == 8:
        import numpy as np                       # only path that needs it
        k = (w + maxw - 1) // maxw
        ch = bpp
        arr = np.frombuffer(pixels[:need], dtype=np.uint8).reshape(h, w, ch)
        hh, ww = (h // k) * k, (w // k) * k
        arr = arr[:hh, :ww].reshape(hh // k, k, ww // k, k, ch).mean(axis=(1, 3))
        arr = np.ascontiguousarray(arr.astype(np.uint8))
        down = {"factor": k, "from": [w, h], "to": [ww // k, hh // k]}
        h, w = arr.shape[0], arr.shape[1]
        pixels = arr.tobytes()
        need = w * h * bpp

    text = None
    if sidecar is not None:
        rec = dict(sidecar)
        rec["converted_by"] = {"tool": "tools/ppm2png.py", "source": os.path.basename(src)}
        if down:
            # A downscaled render is NOT the render the numbers were measured
            # on; say so in the record rather than letting the size imply it.
            rec["converted_by"]["downscale"] = down
        text = json.dumps(rec, ensure_ascii=True)
    else:
        print(f"[ppm2png] {src}: no sidecar — PNG written WITHOUT provenance",
              file=sys.stderr)

    _write_png(dst, w, h, bitdepth, colortype, pixels[:need], bpp, text)
    print(f"{dst}  {w}x{h} {kind}"
          f"{'  downscaled x%d' % down['factor'] if down else ''}"
          f"  provenance={'embedded' if text else 'NONE'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
