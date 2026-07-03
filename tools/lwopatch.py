#!/usr/bin/env python3
"""lwopatch — patch surface (SURF) material values in LightWave LWO1 (LWOB) files.

The write-back half of the LWO surface editor (see tools/editor_server.py):
the browser editor edits live engine Materials; saving maps those values back
into the authoring .lwo files here, then tools/lwsread regenerates the FLD.

Values are given on the ENGINE scale (what the editor shows) and converted to
the on-disk LWO encoding, writing BOTH the legacy integer chunk and its float
twin (LightWave writes both; the converter's float reader wins by file order):

  prop          engine scale        LWO chunks written
  ------------  ------------------  ---------------------------------------
  diffuse       0..1                DIFF u16 (256=100%) + VDIF f32 (0..1)
  specular      0..1                SPEC u16            + VSPC f32
  luminosity    0..1                LUMI u16            + VLUM f32
  transparency  0..100 (percent)    TRAN u16            + VTRN f32
  reflection    0..100 (percent)    REFL u16            + VRFL f32
  glossiness    raw u16 exponent    GLOS u16
  color         R,G,B 0..255        COLR 3 bytes + pad

Missing subchunks are inserted at their canonical position (the order LightWave
itself writes); existing ones are updated in place. Everything else in the file
is preserved byte-for-byte — run with no --set to verify (identity self-test).

Usage:
  lwopatch.py file.lwo --list
  lwopatch.py file.lwo --set 'rooms:specular=0.1' --set 'rooms:glossiness=64' \
              [--backup-dir DIR] [--dry-run] [-o OUT]
"""

import argparse
import datetime
import os
import shutil
import struct
import sys

# Canonical SURF subchunk order (as LightWave 5.x writes them). Insertion
# places a new subchunk before the first existing one that sorts later.
CANON = ["COLR", "FLAG",
         "LUMI", "VLUM", "DIFF", "VDIF", "SPEC", "VSPC", "GLOS",
         "REFL", "VRFL", "TRAN", "VTRN",
         "RFLT", "RIMG", "RSAN", "RIND", "EDGE", "SMAN",
         "CTEX", "DTEX", "STEX", "RTEX", "TTEX", "BTEX",
         "TIMG", "TFLG", "TSIZ", "TCTR", "TFAL", "TVEL", "TCLR", "TVAL",
         "TAMP", "TFRQ", "TALP", "TWRP", "TAAS", "TOPC", "TIP0", "TFP0", "TFP1"]
CANON_POS = {c: i for i, c in enumerate(CANON)}

# prop -> (int_chunk, float_chunk, engine->fraction divisor)
VALUE_PROPS = {
    "diffuse":      ("DIFF", "VDIF", 1.0),
    "specular":     ("SPEC", "VSPC", 1.0),
    "luminosity":   ("LUMI", "VLUM", 1.0),
    "transparency": ("TRAN", "VTRN", 100.0),
    "reflection":   ("REFL", "VRFL", 100.0),
}


class Surf:
    def __init__(self, name, subchunks):
        self.name = name            # str
        self.subchunks = subchunks  # list[(id:str, body:bytes)]

    def _find(self, cid):
        for i, (c, _) in enumerate(self.subchunks):
            if c == cid:
                return i
        return -1

    def set_chunk(self, cid, body):
        """Update in place, or insert at the canonical position."""
        i = self._find(cid)
        if i >= 0:
            self.subchunks[i] = (cid, body)
            return
        pos = CANON_POS.get(cid, len(CANON))
        at = len(self.subchunks)
        for j, (c, _) in enumerate(self.subchunks):
            if CANON_POS.get(c, len(CANON)) > pos:
                at = j
                break
        self.subchunks.insert(at, (cid, body))

    # UV mapping subchunks: CTEX = projection string ("Planar Image Map"...),
    # TFLG = u2 axis/flags bitfield (1=X 2=Y 4=Z + world/pixel-blend bits),
    # TSIZ = 3×f4 world-units-per-tile. The engine bakes UVs from exactly
    # these at FLD load (Get_UV), so patching them + reconverting reproduces
    # the editor's live re-projection.
    UV_PROJ_NAMES = ["Planar Image Map", "Cylindrical Image Map",
                     "Spherical Image Map", "Cubic Image Map"]

    def set_uv_mapping(self, proj, sx, sy, sz, axis):
        name = self.UV_PROJ_NAMES[int(proj)]
        body = name.encode("latin-1") + b"\x00"
        if len(body) % 2:
            body += b"\x00"
        self.set_chunk("CTEX", body)
        self.set_chunk("TSIZ", struct.pack(">3f", float(sx), float(sy), float(sz)))
        i = self._find("TFLG")
        flags = struct.unpack(">H", self.subchunks[i][1])[0] if i >= 0 else 0
        flags = (flags & ~0x7) | (int(axis) & 0x7)
        self.set_chunk("TFLG", struct.pack(">H", flags))

    def set_prop(self, prop, value):
        if prop in VALUE_PROPS:
            ichunk, fchunk, div = VALUE_PROPS[prop]
            frac = float(value) / div
            iv = max(0, min(0xFFFF, round(frac * 256.0)))
            self.set_chunk(ichunk, struct.pack(">H", iv))
            self.set_chunk(fchunk, struct.pack(">f", frac))
        elif prop == "glossiness":
            self.set_chunk("GLOS", struct.pack(">H", max(0, min(0xFFFF, round(float(value))))))
        elif prop in ("baseR", "baseG", "baseB"):
            i = self._find("COLR")
            body = bytearray(self.subchunks[i][1] if i >= 0 else b"\xc8\xc8\xc8\x00")
            body["baseRbaseGbaseB".index(prop) // 5] = max(0, min(255, round(float(value))))
            self.set_chunk("COLR", bytes(body))
        else:
            raise ValueError(f"unknown prop '{prop}'")

    def serialize(self):
        out = self.name.encode("latin-1") + b"\x00"
        if len(out) % 2:
            out += b"\x00"
        for cid, body in self.subchunks:
            out += cid.encode("ascii") + struct.pack(">H", len(body)) + body
            if len(body) % 2:
                out += b"\x00"
        return out


class LwoFile:
    """FORM/LWOB container: SURF chunks are parsed, everything else kept raw."""

    def __init__(self, path):
        self.path = path
        data = open(path, "rb").read()
        if data[0:4] != b"FORM" or data[8:12] != b"LWOB":
            raise ValueError(f"{path}: not a FORM/LWOB (LWO1) file")
        self.chunks = []   # list[("SURF", Surf) | (id:str, raw_body:bytes)]
        off = 12
        end = 8 + struct.unpack(">I", data[4:8])[0]
        while off < end:
            cid = data[off:off + 4].decode("ascii")
            ln = struct.unpack(">I", data[off + 4:off + 8])[0]
            body = data[off + 8:off + 8 + ln]
            if cid == "SURF":
                self.chunks.append(("SURF", self._parse_surf(body)))
            else:
                self.chunks.append((cid, body))
            off += 8 + ln + (ln & 1)

    @staticmethod
    def _parse_surf(body):
        z = body.index(b"\x00")
        name = body[:z].decode("latin-1")
        p = z + 1
        if p % 2:
            p += 1
        subs = []
        while p + 6 <= len(body):
            cid = body[p:p + 4].decode("ascii")
            ln = struct.unpack(">H", body[p + 4:p + 6])[0]
            subs.append((cid, body[p + 6:p + 6 + ln]))
            p += 6 + ln + (ln & 1)
        return Surf(name, subs)

    def surfaces(self):
        return [c[1] for c in self.chunks if c[0] == "SURF"]

    def surface(self, name):
        for s in self.surfaces():
            if s.name == name:
                return s
        return None

    def serialize(self):
        out = b""
        for cid, c in self.chunks:
            body = c.serialize() if cid == "SURF" else c
            out += cid.encode("ascii") + struct.pack(">I", len(body)) + body
            if len(body) % 2:
                out += b"\x00"
        return b"FORM" + struct.pack(">I", len(out) + 4) + b"LWOB" + out


def backup(path, backup_dir):
    os.makedirs(backup_dir, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    dst = os.path.join(backup_dir, f"{stem}.{ts}{ext}")
    n = 0
    while os.path.exists(dst):   # same-second saves
        n += 1
        dst = os.path.join(backup_dir, f"{stem}.{ts}-{n}{ext}")
    shutil.copy2(path, dst)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file")
    ap.add_argument("--set", action="append", default=[],
                    metavar="SURF:PROP=VALUE",
                    help="e.g. 'rooms:specular=0.1' (engine scale; repeatable)")
    ap.add_argument("--list", action="store_true", help="list surfaces + values")
    ap.add_argument("--backup-dir", help="copy the original here before writing")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("-o", "--out", help="write to OUT instead of in place")
    args = ap.parse_args()

    lwo = LwoFile(args.file)
    original = open(args.file, "rb").read()

    # Identity self-test: reserialization with no edits must be byte-exact,
    # otherwise our understanding of the container is wrong — refuse to write.
    if lwo.serialize() != original:
        sys.exit(f"{args.file}: identity reserialize differs — refusing to patch")

    if args.list:
        for s in lwo.surfaces():
            vals = {c: b.hex() for c, b in s.subchunks
                    if c in ("COLR", "DIFF", "VDIF", "SPEC", "VSPC", "GLOS",
                             "LUMI", "VLUM", "TRAN", "VTRN", "REFL", "VRFL")}
            print(f"{s.name}: {vals}")
        return

    touched = False
    for spec in args.set:
        surf_name, _, kv = spec.rpartition(":")
        prop, _, value = kv.partition("=")
        if not surf_name or not value:
            sys.exit(f"bad --set '{spec}' (want SURF:PROP=VALUE)")
        s = lwo.surface(surf_name)
        if s is None:
            sys.exit(f"{args.file}: no surface '{surf_name}' "
                     f"(has: {', '.join(x.name for x in lwo.surfaces())})")
        s.set_prop(prop, value)
        touched = True

    out = lwo.serialize()
    dest = args.out or args.file
    if args.dry_run:
        print(f"dry-run: would write {len(out)} bytes to {dest}"
              f" ({'changed' if out != original else 'IDENTICAL'})")
        return
    if not touched and dest == args.file:
        print(f"{args.file}: identity OK ({len(out)} bytes), nothing to write")
        return
    if args.backup_dir and dest == args.file:
        print(f"backup: {backup(args.file, args.backup_dir)}")
    with open(dest, "wb") as f:
        f.write(out)
    print(f"wrote {dest} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
