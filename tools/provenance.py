"""Shared helpers for snapshot provenance (see FDS/Base/Provenance.cpp).

THE CONTRACT
------------
The engine writes `<stem>.json` beside every PPM it dumps. When a PPM is
converted to PNG (tools/ppm2png.py) that JSON is embedded in the PNG as a
`tEXt` chunk with keyword `groundwork-provenance`, so the PNG is
self-describing once it leaves the directory its sidecar lived in — which is
exactly what happens when a render is copied into docs/img/ and linked into a
report.

Derived images (diffs, side-by-side pairs) get a record naming BOTH parents:

    {"schema": "revival/derived-provenance",
     "schema_version": 1,
     "derived": {"tool": "...", "argv": [...], "written_utc": "..."},
     "inputs": [ {"path": "...", "provenance": {...}|null}, ... ]}

`provenance: null` is written on purpose and is not an omission — it records
that the input carried no provenance, which is a fact about that input.

Nothing here imports PIL or numpy: the PNG chunk surgery is stdlib struct+zlib
so a tool can embed provenance without taking on a dependency, and so the
chunk is written byte-exactly rather than through an encoder's text handling.
"""

from __future__ import annotations

import binascii
import datetime
import json
import os
import struct
import sys
import zlib

KEYWORD = "groundwork-provenance"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


# ── sidecars ────────────────────────────────────────────────────────────

def sidecar_path(image_path: str) -> str:
    """`a/b/x.ppm` -> `a/b/x.json`; a dotted DIRECTORY is left alone."""
    head, tail = os.path.split(image_path)
    stem = tail.rsplit(".", 1)[0] if "." in tail else tail
    return os.path.join(head, stem + ".json")


def read_sidecar(image_path: str):
    """The engine-written sidecar for an image, or None."""
    p = sidecar_path(image_path)
    if not os.path.exists(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        print(f"[prov] {p}: unreadable ({e})", file=sys.stderr)
        return None


# ── PNG chunk surgery ───────────────────────────────────────────────────

def _iter_chunks(blob: bytes):
    if not blob.startswith(PNG_MAGIC):
        raise ValueError("not a PNG")
    off = len(PNG_MAGIC)
    while off + 8 <= len(blob):
        (length,) = struct.unpack(">I", blob[off:off + 4])
        ctype = blob[off + 4:off + 8]
        data = blob[off + 8:off + 8 + length]
        yield off, ctype, data, off + 12 + length
        off += 12 + length


def _chunk(ctype: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + ctype + data
            + struct.pack(">I", binascii.crc32(ctype + data) & 0xFFFFFFFF))


def png_read_provenance(png_path: str):
    """Decode the groundwork-provenance chunk of a PNG. None when absent.

    Reads tEXt, zTXt and iTXt so a chunk written by another tool still round
    trips. Returns the parsed object, or the raw string when it is not JSON.
    """
    try:
        blob = open(png_path, "rb").read()
    except OSError:
        return None
    try:
        for _off, ctype, data, _end in _iter_chunks(blob):
            if ctype == b"tEXt":
                key, _, val = data.partition(b"\x00")
                if key.decode("latin-1") == KEYWORD:
                    return _maybe_json(val.decode("latin-1"))
            elif ctype == b"zTXt":
                key, _, rest = data.partition(b"\x00")
                if key.decode("latin-1") == KEYWORD and rest[:1] == b"\x00":
                    return _maybe_json(zlib.decompress(rest[1:]).decode("latin-1"))
            elif ctype == b"iTXt":
                parts = data.split(b"\x00", 5)
                if len(parts) == 6 and parts[0].decode("utf-8") == KEYWORD:
                    txt = zlib.decompress(parts[5]) if parts[1] == b"\x01" else parts[5]
                    return _maybe_json(txt.decode("utf-8"))
            elif ctype == b"IEND":
                break
    except (ValueError, zlib.error):
        return None
    return None


def _maybe_json(s: str):
    try:
        return json.loads(s)
    except ValueError:
        return s


def png_embed_provenance(png_path: str, obj) -> bool:
    """Insert (or replace) the groundwork-provenance tEXt chunk in place.

    Written as a post-process rather than through an encoder so the existing
    PIL-based tools need one line to become provenance-carrying. The chunk goes
    immediately after IHDR: ancillary text before IDAT is valid PNG and keeps
    the record readable from the first few KB of the file.

    Returns False (with a warning) rather than raising, so a provenance failure
    can never lose the image a tool was actually asked to produce.
    """
    if obj is None:
        return False
    text = obj if isinstance(obj, str) else json.dumps(obj, ensure_ascii=True,
                                                       sort_keys=False)
    try:
        blob = open(png_path, "rb").read()
        out = bytearray(PNG_MAGIC)
        inserted = False
        for _off, ctype, data, _end in _iter_chunks(blob):
            if ctype == b"tEXt" and data.partition(b"\x00")[0].decode("latin-1") == KEYWORD:
                continue                      # drop a stale record
            out += _chunk(ctype, data)
            if ctype == b"IHDR" and not inserted:
                out += _chunk(b"tEXt", KEYWORD.encode("latin-1") + b"\x00"
                              + text.encode("latin-1", "replace"))
                inserted = True
        if not inserted:
            return False
        open(png_path, "wb").write(bytes(out))
        return True
    except (OSError, ValueError) as e:
        print(f"[prov] {png_path}: could not embed provenance ({e})", file=sys.stderr)
        return False


# ── records ─────────────────────────────────────────────────────────────

def _utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def provenance_of(path: str):
    """Whatever provenance an image carries: PNG chunk, else PPM sidecar."""
    if path.lower().endswith(".png"):
        return png_read_provenance(path)
    return read_sidecar(path)


def derived_record(tool: str, inputs, argv=None) -> dict:
    """A record for an image derived from one or more provenance-bearing ones."""
    return {
        "schema": "revival/derived-provenance",
        "schema_version": 1,
        "derived": {
            "tool": tool,
            "argv": list(argv if argv is not None else sys.argv),
            "cwd": os.getcwd(),
            "written_utc": _utc_now(),
        },
        "inputs": [{"path": os.path.abspath(p), "provenance": provenance_of(p)}
                   for p in inputs],
    }


def carry_through(out_png: str, tool: str, inputs, argv=None) -> bool:
    """Embed a derived record into out_png. No-op when no input had provenance.

    A record whose every input is null says nothing the filename does not, and
    writing one would make un-provenanced images LOOK provenanced — the exact
    confusion this whole feature exists to end.
    """
    rec = derived_record(tool, inputs, argv)
    if all(i["provenance"] is None for i in rec["inputs"]):
        return False
    return png_embed_provenance(out_png, rec)
