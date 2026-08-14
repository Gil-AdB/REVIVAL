#!/usr/bin/env python3
"""Generate the PBR/env-reflection test scene sources (Authoring/pbrtest/).

Emits LightWave 5.x-era files that tools/lwsread converts to PBRTEST.FLD:
  floor.lwo   40x40 ground quad grid   surface "floor"   (planar-Y UV)
  wall.lwo    back + side wall quads   surface "wall"    (planar-Z/X UV)
  balls.lwo   6 UV-spheres in a row    surfaces ball_gloss16..ball_refl80
              sweeping glossiness (16/64/256) and reflection (0/40/80)
  box.lwo     axis-aligned cube        surface "box"     (cubic UV)
  PBRTEST.LWS 3 point lights (key/warm/cool, LightRange set — FLD omnis
              default Range to 0 and light NOTHING in deferred otherwise),
              camera on a 2-key dolly (t=0..240) so editor play has motion.
Also writes the neutral checker texture Runtime/TEXTURES/PBRTEST.PNG that
every surface references (textured materials are what the PBR import path
replaces; untextured mats take the BaseCol code path instead).

Deterministic output — rerunning must reproduce the same bytes, so the FLD
regen stays reproducible (same contract as the other Authoring/ scenes).

Usage: tools/make_pbrtest.py   (writes into the repo, from anywhere)
"""
import math
import os
import struct

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "Authoring", "pbrtest")

# ── LWO1 writer ──────────────────────────────────────────────────────────

def _padstr(s):
    b = s.encode("latin-1") + b"\0"
    if len(b) & 1:
        b += b"\0"
    return b

def _sub(cid, body):
    return cid.encode() + struct.pack(">H", len(body)) + body + (b"\0" if len(body) & 1 else b"")

def _chunk(cid, body):
    return cid.encode() + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

class Surf:
    """SURF description; ints are LW scale (256 = 100%), gloss = LW int."""
    def __init__(self, name, colr=(200, 200, 200), diff=256, spec=0, glos=0,
                 refl=0, sman_deg=0.0, ctex=None, timg=None, tflg=0, tsiz=None):
        self.name = name
        self.colr, self.diff, self.spec, self.glos, self.refl = colr, diff, spec, glos, refl
        self.sman_deg = sman_deg
        self.ctex, self.timg, self.tflg, self.tsiz = ctex, timg, tflg, tsiz

    def encode(self):
        # Int chunks (DIFF/SPEC/REFL) exist for LW-compat but the 1998
        # reader's int path is broken (reads 1 byte of the u16 → ~0) —
        # the FLOAT twins (VDIF/VSPC/VRFL) are what actually convert, so
        # always write both, float AFTER int (file order wins), exactly
        # like LightWave itself does.
        b = _padstr(self.name)
        b += _sub("COLR", bytes(self.colr) + b"\0")
        b += _sub("FLAG", struct.pack(">H", 0x0004))
        b += _sub("DIFF", struct.pack(">H", self.diff))
        b += _sub("VDIF", struct.pack(">f", self.diff / 256.0))
        if self.spec:
            b += _sub("SPEC", struct.pack(">H", self.spec))
            b += _sub("VSPC", struct.pack(">f", self.spec / 256.0))
        if self.glos:
            b += _sub("GLOS", struct.pack(">H", self.glos))
        if self.refl:
            b += _sub("REFL", struct.pack(">H", self.refl))
            b += _sub("VRFL", struct.pack(">f", self.refl / 256.0))
        if self.sman_deg > 0.0:
            b += _sub("SMAN", struct.pack(">f", math.radians(self.sman_deg)))
        if self.ctex:
            b += _sub("CTEX", _padstr(self.ctex))
            b += _sub("TIMG", _padstr(self.timg))
            b += _sub("TWRP", struct.pack(">HH", 2, 2))
            b += _sub("TFLG", struct.pack(">H", self.tflg))
            b += _sub("TSIZ", struct.pack(">fff", *self.tsiz))
            b += _sub("TCTR", struct.pack(">fff", 0.0, 0.0, 0.0))
        return _chunk("SURF", b)

def write_lwo(path, verts, polys, surfs):
    """verts: [(x,y,z)], polys: [(surf_index_1based, [vert indices])]."""
    pnts = b"".join(struct.pack(">fff", *v) for v in verts)
    srfs = b"".join(_padstr(s.name) for s in surfs)
    pols = b""
    for sidx, vs in polys:
        pols += struct.pack(">H", len(vs))
        pols += b"".join(struct.pack(">H", v) for v in vs)
        pols += struct.pack(">h", sidx)
    body = _chunk("PNTS", pnts) + _chunk("SRFS", srfs) + _chunk("POLS", pols)
    body += b"".join(s.encode() for s in surfs)
    data = b"FORM" + struct.pack(">I", len(body) + 4) + b"LWOB" + body
    with open(path, "wb") as f:
        f.write(data)
    print(f"  {os.path.basename(path)}  {len(data)} bytes, {len(verts)} verts, {len(polys)} polys, {len(surfs)} surfs")

# ── geometry ─────────────────────────────────────────────────────────────

def grid_quads(x0, x1, y0, y1, z0, z1, nx, ny, plane, flip=False):
    """Subdivided quad grid on one plane ('y': ground; 'z': back wall; 'x')."""
    verts, polys = [], []
    for j in range(ny + 1):
        for i in range(nx + 1):
            u = x0 + (x1 - x0) * i / nx
            v = y0 + (y1 - y0) * j / ny
            if plane == "y":
                verts.append((u, z0, v))
            elif plane == "z":
                verts.append((u, v, z0))
            else:
                verts.append((z0, v, u))
    for j in range(ny):
        for i in range(nx):
            a = j * (nx + 1) + i
            q = [a, a + 1, a + nx + 2, a + nx + 1]
            if flip:
                q.reverse()
            polys.append(q)
    return verts, polys

def uv_sphere(cx, cy, cz, r, nseg=16, nring=12):
    verts = [(cx, cy + r, cz)]
    for j in range(1, nring):
        phi = math.pi * j / nring
        for i in range(nseg):
            th = 2.0 * math.pi * i / nseg
            verts.append((cx + r * math.sin(phi) * math.sin(th),
                          cy + r * math.cos(phi),
                          cz + r * math.sin(phi) * math.cos(th)))
    verts.append((cx, cy - r, cz))
    polys = []
    def ring(j):                       # first vertex index of ring j (1-based rings)
        return 1 + (j - 1) * nseg
    # Winding: outward-facing for the engine's cull/normal convention —
    # the first cut had these reversed and the spheres rendered INSIDE-OUT
    # (visible, but lit as if the normals pointed into the ball: lights
    # above produced bottom-lit spheres — "normals upside down").
    for i in range(nseg):
        polys.append([0, ring(1) + i, ring(1) + (i + 1) % nseg])
    for j in range(1, nring - 1):
        for i in range(nseg):
            a = ring(j) + i
            b = ring(j) + (i + 1) % nseg
            c = ring(j + 1) + (i + 1) % nseg
            d = ring(j + 1) + i
            polys.append([d, c, b, a])
    last = len(verts) - 1
    for i in range(nseg):
        polys.append([last, ring(nring - 1) + (i + 1) % nseg, ring(nring - 1) + i])
    return verts, polys

def box(cx, cy, cz, sx, sy, sz):
    x0, x1 = cx - sx / 2, cx + sx / 2
    y0, y1 = cy - sy / 2, cy + sy / 2
    z0, z1 = cz - sz / 2, cz + sz / 2
    v = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),
         (x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    f = [[3,2,1,0],[6,7,4,5],[7,3,0,4],[2,6,5,1],[7,6,2,3],[0,1,5,4]]
    return v, f

# ── build the objects ────────────────────────────────────────────────────

def main():
    os.makedirs(OUT, exist_ok=True)
    print("[pbrtest] writing", OUT)
    TIMG = "PBRTEST.PNG"

    # flip=True: engine cull-normal sign is path-dependent (probe + flip)
    # — the unflipped ground faced DOWN and rendered black from above.
    fv, fp = grid_quads(-20, 20, -20, 20, 0, 0, 8, 8, "y", flip=True)
    write_lwo(os.path.join(OUT, "floor.lwo"), fv,
              [(1, q) for q in fp],
              [Surf("floor", (190, 190, 190), spec=77, glos=64,
                    ctex="Planar Image Map", timg=TIMG, tflg=0x2, tsiz=(10.0, 10.0, 10.0))])

    wv, wp = grid_quads(-20, 20, 0, 14, 20, 0, 8, 3, "z", flip=True)
    sv, sp = grid_quads(-20, 20, 0, 14, -20, 0, 8, 3, "x", flip=True)
    write_lwo(os.path.join(OUT, "wall.lwo"), wv + sv,
              [(1, q) for q in wp] + [(1, [i + len(wv) for i in q]) for q in sp],
              [Surf("wall", (170, 175, 185), spec=51, glos=64,
                    ctex="Planar Image Map", timg=TIMG, tflg=0x4, tsiz=(8.0, 8.0, 8.0))])

    # Six spheres: left triple sweeps GLOSSINESS (matte->tight highlight) at
    # spec 60%, right triple sweeps REFLECTION 0/40/80 at gloss 256.
    ball_surfs = [
        Surf("ball_gloss16",  (200, 60, 60),  spec=154, glos=16,  sman_deg=89.0,
             ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0)),
        Surf("ball_gloss64",  (60, 200, 60),  spec=154, glos=64,  sman_deg=89.0,
             ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0)),
        Surf("ball_gloss256", (60, 60, 200),  spec=154, glos=256, sman_deg=89.0,
             ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0)),
        Surf("ball_refl0",    (200, 170, 60), spec=154, glos=256, refl=0,   sman_deg=89.0,
             ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0)),
        Surf("ball_refl40",   (180, 180, 180),spec=154, glos=256, refl=102, sman_deg=89.0,
             ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0)),
        Surf("ball_refl80",   (220, 220, 220),spec=154, glos=256, refl=205, sman_deg=89.0,
             ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0)),
    ]
    bv, bp = [], []
    for k in range(6):
        v, p = uv_sphere(-12.5 + 5.0 * k, 2.2, 6.0, 1.8)
        base = len(bv)
        bv += v
        bp += [(k + 1, [i + base for i in q]) for q in p]
    write_lwo(os.path.join(OUT, "balls.lwo"), bv, bp, ball_surfs)

    xv, xp = box(14.0, 1.5, 0.0, 3.0, 3.0, 3.0)
    write_lwo(os.path.join(OUT, "box.lwo"), xv, [(1, q) for q in xp],
              [Surf("box", (160, 140, 120), spec=102, glos=64,
                    ctex="Cubic Image Map", timg=TIMG, tflg=0x1, tsiz=(3.0, 3.0, 3.0))])

    # ── LWS scene ────────────────────────────────────────────────────────
    def motion(x, y, z, h=0.0, p=0.0):
        return (f"  9\n  1\n  {x:g} {y:g} {z:g} {h:g} {p:g} 0 1 1 1\n  0 0 0 0 0\n")
    lws = "LWSC\n1\n\n"
    lws += "FirstFrame 0\nLastFrame 240\nFrameStep 1\n"
    lws += "PreviewFirstFrame 0\nPreviewLastFrame 240\nPreviewFrameStep 1\nFramesPerSecond 30.000000\n\n"
    # AmbientColor/AmbIntensity are MANDATORY: FLDSAVE writes those envelopes
    # unconditionally and segfaults on the null pointers if the LWS omits them.
    lws_ambient = "AmbientColor 255 255 255\nAmbIntensity 0.250000\n\n"
    for obj in ("floor", "wall", "balls", "box"):
        lws += f"LoadObject {obj}.lwo\nShowObject 8 7\nObjectMotion (unnamed)\n"
        lws += motion(0, 0, 0)
        lws += "EndBehavior 1\nShadowOptions 7\n\n"
    lights = [
        ("key",  (0.0, 12.0, -8.0), (255, 255, 255), 1.0, 80.0),
        ("warm", (-14.0, 6.0, 2.0), (255, 190, 120), 0.8, 50.0),
        ("cool", (14.0, 5.0, 10.0), (120, 170, 255), 0.7, 45.0),
    ]
    lws += lws_ambient
    for name, pos, col, inten, rng in lights:
        lws += f"AddLight\nLightName {name}\nShowLight 1 7\nLightMotion (unnamed)\n"
        lws += motion(*pos)
        lws += "EndBehavior 1\n"
        lws += f"LightColor {col[0]} {col[1]} {col[2]}\n"
        lws += f"LgtIntensity {inten:.6f}\nLightRange {rng:g}\nLightType 1\nShadowType 1\n\n"
    # Camera: slow 2-key dolly so editor play mode shows motion.
    lws += "ShowCamera 1 7\nCameraMotion (unnamed)\n  9\n  2\n"
    lws += "  0 5.5 -17 0 8 0 1 1 1\n  0 0 0 0 0\n"
    lws += "  6 4.5 -13 -18 6 0 1 1 1\n  240 0 0 0 0\n"
    lws += "EndBehavior 1\nZoomFactor 3.200000\n"
    with open(os.path.join(OUT, "PBRTEST.LWS"), "w") as f:
        f.write(lws)
    print("  PBRTEST.LWS")

    # ── neutral checker texture ──────────────────────────────────────────
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (256, 256), (168, 168, 168))
    d = ImageDraw.Draw(img)
    for j in range(8):
        for i in range(8):
            if (i + j) & 1:
                d.rectangle([i * 32, j * 32, i * 32 + 31, j * 32 + 31], fill=(120, 120, 124))
    for k in range(0, 257, 32):
        d.line([(k, 0), (k, 256)], fill=(90, 90, 94))
        d.line([(0, k), (256, k)], fill=(90, 90, 94))
    tex = os.path.join(REPO, "Runtime", "TEXTURES", "PBRTEST.PNG")
    img.save(tex)
    print("  " + os.path.relpath(tex, REPO))

if __name__ == "__main__":
    main()
