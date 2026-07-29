#!/usr/bin/env python3
"""obj2lwo — convert a Wavefront OBJ into a LightWave LWO1 (FORM..LWOB) object
that tools/lwsread/LWOREAD.CPP parses and the Flood engine renders.

Written for the chase lighthouse beacon (a CC0 low-poly lighthouse OBJ → a
LoadObject'd tower), but generic: any triangulated/quad OBJ with usemtl groups
converts. Each OBJ material becomes one LWO SURF surface.

Why a texture per surface is MANDATORY: the deferred surface kernel SKIPS any
material whose Txtr is null (DeferredSurfaceKernel.cpp:1512) — untextured opaque
geometry never gets shaded, so it renders INVISIBLE. Every mapped surface here
therefore carries a TIMG (+ a Cubic projection, mirroring the chase mountains'
SURF encoding). Point them at small solid-colour JPGs in Runtime/TEXTURES for a
flat classic look (white tower / cool lamp / dark door) that still renders.

Coordinate convention: lwsread's SwapYZ is a NO-OP, so on-disk LWO X/Y/Z map
straight to engine coords with Y up. Blender's OBJ export is already Y-up, so no
axis swap is needed; --recenter drops the base to Y=0 and centres the X/Z
footprint on the origin (the source mesh sits at a world offset). Native size is
preserved — scale the instance in the LWS LoadObject key.

Usage:
  obj2lwo.py IN.obj OUT.lwo [--recenter] [--flip-winding]
      --map "OBJMAT=surfname,R,G,B,vlum,vdif,TEX.JPG"   (repeatable, one per material)

  vlum/vdif are on-disk 0..1 fractions (lwsread_legacy stores Luminosity =
  vlum*100, Diffuse = vdif*100 — same twin encoding as tools/lwopatch.py). A
  material with no --map falls back to its MTL Kd colour + a shared --default-tex.
"""
import sys, os, struct, argparse


def _chunk(cid, body):
    return cid + struct.pack(">I", len(body)) + body + (b"\x00" if len(body) & 1 else b"")


def _asciiz(s):
    b = s.encode("latin-1") + b"\x00"
    return b + (b"\x00" if len(b) & 1 else b"")


def _sub(sid, body):
    return sid + struct.pack(">H", len(body)) + body + (b"\x00" if len(body) & 1 else b"")


def _surf(name, colr, vlum, vdif, tex):
    """SURF body: name + COLR/FLAG/LUMI/VLUM/DIFF/VDIF + Cubic texture block.
    The texture makes Mat->Txtr non-null so the deferred kernel shades it."""
    b = _sub(b"COLR", bytes([colr[0], colr[1], colr[2], 0]))
    b += _sub(b"FLAG", struct.pack(">H", 1 if vlum > 0 else 0))   # Surf_Luminous when emissive
    if vlum > 0:
        b += _sub(b"LUMI", struct.pack(">H", max(0, min(0xFFFF, round(vlum * 256)))))
        b += _sub(b"VLUM", struct.pack(">f", vlum))
    b += _sub(b"DIFF", struct.pack(">H", max(0, min(0xFFFF, round(vdif * 256)))))
    b += _sub(b"VDIF", struct.pack(">f", vdif))
    # Cubic projection block, mirroring the chase mountains' SURF (m1.lwo). For a
    # solid-colour texture the projection is cosmetically irrelevant, but a valid
    # CTEX/TIMG/TFLG/TSIZ/TCTR set is what makes Get_Mapping + the loader wire the
    # Texture up. TIMG basename resolves against Runtime/TEXTURES.
    b += _sub(b"CTEX", _asciiz("Cubic Image Map"))
    b += _sub(b"TIMG", _asciiz(tex))
    b += _sub(b"TWRP", struct.pack(">HH", 2, 2))
    b += _sub(b"TFLG", struct.pack(">H", 0x0021))
    b += _sub(b"TSIZ", struct.pack(">3f", 100.0, 100.0, 100.0))
    b += _sub(b"TCTR", struct.pack(">3f", 0.0, 0.0, 0.0))
    return _asciiz(name) + b


def parse_obj(path):
    verts, faces = [], []      # faces: (vertidx-list 0based, matname)
    cur = None
    mats_order = []
    for ln in open(path, encoding="latin-1"):
        if ln.startswith("v "):
            p = ln.split()
            verts.append((float(p[1]), float(p[2]), float(p[3])))
        elif ln.startswith("usemtl"):
            cur = ln.split()[1] if len(ln.split()) > 1 else "default"
            if cur not in mats_order:
                mats_order.append(cur)
        elif ln.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in ln.split()[1:]]
            faces.append((idx, cur))
    return verts, faces, mats_order


def parse_mtl_kd(path):
    kd, cur = {}, None
    if not os.path.exists(path):
        return kd
    for ln in open(path, encoding="latin-1"):
        if ln.startswith("newmtl"):
            cur = ln.split()[1]
        elif ln.startswith("Kd") and cur:
            p = ln.split()
            kd[cur] = tuple(max(0, min(255, round(float(x) * 255))) for x in p[1:4])
    return kd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("lwo")
    ap.add_argument("--recenter", action="store_true",
                    help="drop base to Y=0 and centre the X/Z footprint on the origin")
    ap.add_argument("--flip-winding", action="store_true",
                    help="reverse polygon vertex order (if faces render inside-out)")
    ap.add_argument("--map", action="append", default=[],
                    metavar="OBJMAT=surf,R,G,B,vlum,vdif,TEX",
                    help="per-material surface (repeatable)")
    ap.add_argument("--default-tex", default="MOUNT.JPG",
                    help="texture for materials with no --map (uses MTL Kd colour)")
    args = ap.parse_args()

    verts, faces, mats_order = parse_obj(args.obj)
    kd = parse_mtl_kd(os.path.splitext(args.obj)[0] + ".mtl")

    # material -> (surfname, colr, vlum, vdif, tex)
    spec = {}
    for m in args.map:
        key, _, rest = m.partition("=")
        f = rest.split(",")
        spec[key] = (f[0], (int(f[1]), int(f[2]), int(f[3])),
                     float(f[4]), float(f[5]), f[6])
    surfaces = []          # ordered (surfname, colr, vlum, vdif, tex)
    surf_index = {}        # objmat -> 1-based surface index
    for m in mats_order:
        if m in spec:
            s = spec[m]
        else:
            s = (m or "surf", kd.get(m, (200, 200, 200)), 0.0, 1.0, args.default_tex)
        surf_index[m] = len(surfaces) + 1
        surfaces.append(s)

    # recenter (X/Z footprint centre, base Y -> 0). Native scale preserved.
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    ox = (min(xs) + max(xs)) / 2 if args.recenter else 0.0
    oy = min(ys) if args.recenter else 0.0
    oz = (min(zs) + max(zs)) / 2 if args.recenter else 0.0
    verts = [(v[0] - ox, v[1] - oy, v[2] - oz) for v in verts]

    pnts = b"".join(struct.pack(">3f", *v) for v in verts)
    srfs = b"".join(_asciiz(s[0]) for s in surfaces)
    pols = b""
    for idx, m in faces:
        vi = list(reversed(idx)) if args.flip_winding else idx
        pols += struct.pack(">H", len(vi))
        pols += b"".join(struct.pack(">H", v) for v in vi)
        pols += struct.pack(">H", surf_index[m])
    body = _chunk(b"PNTS", pnts) + _chunk(b"SRFS", srfs) + _chunk(b"POLS", pols)
    for s in surfaces:
        body += _chunk(b"SURF", _surf(*s))
    data = b"FORM" + struct.pack(">I", len(body) + 4) + b"LWOB" + body
    with open(args.lwo, "wb") as fh:
        fh.write(data)

    # report the per-surface local-Y extents (the beam anchor = lamp centre)
    print("wrote %s: %d verts, %d faces, %d surfaces" % (args.lwo, len(verts), len(faces), len(surfaces)))
    for si, s in enumerate(surfaces, 1):
        sy = [verts[v][1] for idx, m in faces if surf_index[m] == si for v in idx]
        if sy:
            print("  surf %d '%s' colr=%s vlum=%s Y %.2f..%.2f (centre %.2f)"
                  % (si, s[0], s[1], s[2], min(sy), max(sy), 0.5 * (min(sy) + max(sy))))


if __name__ == "__main__":
    main()
