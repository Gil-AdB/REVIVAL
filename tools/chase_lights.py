#!/usr/bin/env python3
"""Author ENVIRONMENTAL-LIGHT DRAMA into Authoring/chase/CHASE.LWS.

PRIMARY variant --runway: a colonnade of ANCHORED, SWEEPING, COLOUR-SHIFTING
red-and-white lighthouses lining the flight lane (auto-placed from Ship1's
ground track, alternating sides, open-water stretches only — the gorge is
skipped). Per beacon:
  * a REAL striped lighthouse tower (lighthouse.lwo, CC0 — see ASSETS.md;
    --runway-model proc regenerates the faceted beacon.lwo fallback), seated
    TERRAIN-AWARE: the footprint samples the mountain meshes and the base is
    buried under the water / inside the island rock (--runway-sink /
    --runway-rock-sink), so island towers stand on the rock, sea towers in the
    water ("lighthouses height need to consider the terrain").
  * a spinning null rotor + spot(s) PARENTED to it → the beam sweeps
    (Transform.cpp:330 re-derives the cone axis per frame; phases staggered).
  * COLOURED beams with DYNAMIC CHANGES: two co-located spots per rotor with
    anti-phase half-wave 'LgtIntensity  (envelope)' keys crossfade the beam
    between a BEAM_PALETTE colour pair (engine evaluates the intensity spline
    every frame — Transform.cpp:255 → ISize → light SoA colour). NO engine
    change; --runway-dual 0 = single-colour breathing beams instead.
Secondary --lighthouse (a slow rotating warm accent, retuned WAY down).
--lasers is the legacy sky-fan gauntlet, tamed hard and no longer in the
default variant set. --realign re-aims the two existing canyon spots.

Family: tools/chase_bank.py / chase_camera.py / chase_loop.py — in-place,
idempotent, parameterized, re-runnable. Authored-first: the result is LWS lights
(+ one null for the lighthouse) that flow through lwsread_legacy into the FLD via
the PROVEN mechanism (LightType 2 spot + ConeAngle + VolumetricLight 1 +
VolumetricLightIntensity → FLD bit-2048 → Omni_ForceVolCone + VolBeamGain; city's
46 headlights prove it end-to-end). NO engine change.

The beams are VOLUMETRIC god-ray shafts. They are visible under the chase
CINEMATIC profile (--cinematic: fast_fog on, cone_strength 2.0, a low fog band
Y[-400,420] hugging the water). In the plain --deferred default (no fog,
cone_strength 0.05) they are near-inert — the same "cinematic-only drama" regime
the city beams live in. Render candidates with --cinematic.

Mechanism notes
  * A LightType-2 spot's cone axis IDir = its local +Z rotated by its authored
    first-key H/P/B — set ONCE at load for a NON-parented spot (static beam), or
    OVERWRITTEN EACH FRAME from the parent's rotation for a PARENTED spot
    (Transform.cpp:330). So a rotating beam = a spot parented to a NULL whose
    heading is animated; a fixed laser = a non-parented spot with authored H/P.
  * P>0 pitches the beam DOWN (validated); H = atan2(dx,dz) toward the aim.

Idempotence
  * All added lights/nulls live between sentinel marker lines
    (FDSLIGHTS_OBJ_BEGIN/END in the object section for the lighthouse null,
    FDSLIGHTS_LGT_BEGIN/END in the light section for the spots) — unknown
    keywords lwsread safely skips (the f92c3c4 fix, same idiom as
    FDSCORRIDOREND). Every run STRIPS those regions first, then re-inserts, so
    re-running never duplicates and --clear reverts cleanly. --realign edits the
    two existing "canyon warm/cool spot" blocks in place from fixed target
    params (recomputed each run → idempotent).

Usage
  tools/chase_lights.py [--runway] [--lighthouse] [--lasers N] [--realign] [--clear]
  tools/chase_lights.py --runway                     # PRIMARY: the runway colonnade
  tools/chase_lights.py --runway --lighthouse        # runway + a warm accent beam
  tools/chase_lights.py --lighthouse                 # rotating beam only (secondary)
  tools/chase_lights.py --clear                      # strip all fds lights

  Runway knobs:      --runway-beacons N  --runway-gain G  --runway-cone DEG
                     --runway-offset U  --runway-turns T  --runway-phase DEG
                     --runway-pitch DEG  --runway-fixed-alt
                     --runway-dual 0|1  --runway-pulse FRAMES
                     --runway-int-hi F  --runway-int-lo F
                     --runway-model lighthouse|proc  --runway-lamp-height U
                     --runway-scale S  --runway-stock S
                     --runway-sink U  --runway-rock-sink U
  Lighthouse knobs:  --lh-pos X Y Z  --lh-pitch DEG  --lh-turns T
                     --lh-gain G  --lh-cone DEG  --lh-color R G B
  Laser knobs:       --laser-gain G  --laser-cone DEG
Then: regen (lwsread_legacy) + install (see Authoring/chase/README.md), or run
./chase_preview.sh {runway|runway+lighthouse|lighthouse|off}.
"""
import os, sys, math, struct, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS  = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")
BEACON_LWO = os.path.join(REPO, "Authoring", "chase", "beacon.lwo")
LIGHTHOUSE_LWO = os.path.join(REPO, "Authoring", "chase", "lighthouse.lwo")

OBJ_BEGIN, OBJ_END = "FDSLIGHTS_OBJ_BEGIN", "FDSLIGHTS_OBJ_END"
LGT_BEGIN, LGT_END = "FDSLIGHTS_LGT_BEGIN", "FDSLIGHTS_LGT_END"


def fmt(v):
    if abs(v) < 1e-9:
        return "0"
    return ("%.4f" % v).rstrip("0").rstrip(".")


def strip_region(lines, begin, end):
    """Remove lines between (and including) the begin/end markers."""
    out, skip = [], False
    for ln in lines:
        s = ln.strip()
        if s == begin:
            skip = True
            continue
        if s == end:
            skip = False
            continue
        if not skip:
            out.append(ln)
    return out


def count_objects(lines):
    return sum(1 for ln in lines
               if ln.strip().startswith(("LoadObject", "AddNullObject")))


def aim_hp(src, tgt):
    """LW heading/pitch (deg) so a spot at `src` shines its +Z at `tgt`.
    P>0 = nose-down (validated against the parented-null beam)."""
    dx, dy, dz = tgt[0] - src[0], tgt[1] - src[1], tgt[2] - src[2]
    horiz = math.hypot(dx, dz) or 1e-6
    H = math.degrees(math.atan2(dx, dz))
    P = math.degrees(math.atan2(-dy, horiz))
    return H, P


# ── flight-path sampling (for the runway: auto-place from Ship1's motion) ─────
FIRST_FRAME, LAST_FRAME = 0, 1760


def parse_ship_keys(lines, want_base):
    """(frame, x, y, z) list for a ship's ObjectMotion — same walker as
    chase_corridor.py / chase_camera.py."""
    n = len(lines); li = 0
    while li < n:
        s = lines[li].strip()
        if s.startswith("LoadObject") and \
                s[len("LoadObject"):].replace("/", "\\").split("\\")[-1].strip().lower() == want_base:
            j = li + 1; mo = None
            while j < n:
                t = lines[j].strip()
                if t.startswith(("LoadObject", "AddLight")):
                    break
                if t.startswith("ObjectMotion"):
                    mo = j; break
                j += 1
            if mo is None:
                sys.exit("no ObjectMotion for %s" % want_base)
            def nxt(m):
                while m < n and lines[m].strip() == "": m += 1
                return m
            k = nxt(mo + 1); k = nxt(k + 1)
            nk = int(lines[k].split()[0]); k = nxt(k + 1)
            keys = []
            for _ in range(nk):
                vals = [float(x) for x in lines[k].split()]
                k2 = nxt(k + 1); fr = int(float(lines[k2].split()[0]))
                keys.append((fr, vals[0], vals[1], vals[2]))
                k = nxt(k2 + 1)
            return keys
        li += 1
    sys.exit("no LoadObject %s" % want_base)


def path_at(keys, f):
    if f <= keys[0][0]:
        return keys[0][1:]
    if f >= keys[-1][0]:
        return keys[-1][1:]
    for i in range(len(keys) - 1):
        a, b = keys[i], keys[i + 1]
        if a[0] <= f <= b[0]:
            t = (f - a[0]) / (b[0] - a[0]) if b[0] != a[0] else 0.0
            return tuple(a[1 + c] + t * (b[1 + c] - a[1 + c]) for c in range(3))
    return keys[-1][1:]


def path_tangent_xz(keys, f):
    a = path_at(keys, max(FIRST_FRAME, f - 10))
    b = path_at(keys, min(LAST_FRAME, f + 10))
    dx, dz = b[0] - a[0], b[2] - a[2]
    L = math.hypot(dx, dz) or 1.0
    return dx / L, dz / L


# ── beacon.lwo generator (a faceted lighthouse pylon + luminous lamp cap) ──────
# The runway spots used to FLOAT (user: "the spot lights are not anchored to
# anything - there should be at least a lighthouse model"). Each beam now
# emanates from a real structure: a tapered octagonal tower standing in the
# water, with a small lamp-house band near the top carrying a luminous material
# so the cap reads as the light source. The beam origin sits AT the lamp height.
#
# Format = LWO1 / FORM..LWOB, exactly what tools/lwsread/LWOREAD.CPP parses
# (big-endian; PNTS/SRFS/POLS + one SURF per surface; NO parser change). SwapYZ
# is a NO-OP in lwsread (body commented out), so on-disk LWO coords map straight
# to engine coords with Y up — the tower is authored base-at-Y=0, apex-at-Y=H.
# Materials mirror the water/mountain SURF encoding (COLR pad byte, DIFF u16 +
# VDIF f32 twin, LUMI u16 + VLUM f32 twin). VLUM is a 0..1 fraction; the legacy
# converter (lwsread_legacy, -DLEGACY_VLUM) stores Luminosity = VLUM*100, and
# the deferred kernel's emissive is Luminosity*BaseCol for an untextured surface
# (DeferredSurfaceKernel.cpp:1742) — so VLUM 0.015 → Luminosity 1.5 → a bright
# cool-white cap, VLUM 0.004 → 0.4 → a faint cool baseline that keeps the shaft
# from going pure-black at night. Untextured diffuse is Diffuse*Ambient
# (colourless), hence the small shaft luminosity to carry the stone tint.
FACETS = 8


def _lwo_chunk(cid, body):
    out = cid + struct.pack(">I", len(body)) + body
    return out + (b"\x00" if len(body) & 1 else b"")


def _lwo_asciiz(s):
    b = s.encode("latin-1") + b"\x00"
    return b + (b"\x00" if len(b) & 1 else b"")


def _lwo_sub(sid, body):
    out = sid + struct.pack(">H", len(body)) + body
    return out + (b"\x00" if len(body) & 1 else b"")


def _surf_body(name, colr, lum_frac, dif_frac, flags):
    """One SURF chunk body: name + canonical subchunks (COLR/FLAG/LUMI/VLUM/
    DIFF/VDIF). VLUM/VDIF float twins are authoritative (read after their int
    chunk). lum_frac/dif_frac are 0..1 engine fractions."""
    subs = _lwo_sub(b"COLR", bytes([colr[0], colr[1], colr[2], 0]))
    subs += _lwo_sub(b"FLAG", struct.pack(">H", flags))
    if lum_frac > 0:
        subs += _lwo_sub(b"LUMI", struct.pack(">H", max(0, min(0xFFFF, round(lum_frac * 256)))))
        subs += _lwo_sub(b"VLUM", struct.pack(">f", lum_frac))
    subs += _lwo_sub(b"DIFF", struct.pack(">H", max(0, min(0xFFFF, round(dif_frac * 256)))))
    subs += _lwo_sub(b"VDIF", struct.pack(">f", dif_frac))
    return _lwo_asciiz(name) + subs


def write_beacon_lwo(path, height, base_r, top_r, lamp_r,
                     tower_col, lamp_col, lamp_lum, shaft_lum):
    """Write a faceted lighthouse LWO. Returns the lamp-house CENTRE local-Y
    (the beam origin height above the tower base). Two surfaces: 'tower'
    (faceted shaft + roof, faint cool baseline so it never goes pure black) and
    'lamp' (the luminous cap band)."""
    F = FACETS
    shaft_top = 0.68 * height     # top of the tapered shaft (radius top_r)
    lamp_top = 0.88 * height      # top of the lamp-house band (radius lamp_r)
    lamp_ctr = 0.5 * (shaft_top + lamp_top)   # beam origin height

    def ring(y, r):
        return [(r * math.cos(2 * math.pi * i / F), y, r * math.sin(2 * math.pi * i / F))
                for i in range(F)]
    r0 = ring(0.0, base_r)         # base ring (widest)
    r1 = ring(shaft_top, top_r)    # shaft top (narrow)
    r2 = ring(lamp_top, lamp_r)    # lamp-house top (flared out)
    apex = (0.0, height, 0.0)      # roof point
    verts = r0 + r1 + r2 + [apex]
    A, B, C, AP = 0, F, 2 * F, 3 * F   # vertex base indices

    # Faces (surf 1-based: 1=tower, 2=lamp). Winding [lo_i, up_i, up_{i+1},
    # lo_{i+1}] gives an OUTWARD normal under the engine's front-face convention
    # (verified against water.lwo's up-facing quad); roof tri [r2_i, apex,
    # r2_{i+1}] follows the same order. No bottom cap (base is underwater).
    faces = []
    for i in range(F):
        j = (i + 1) % F
        faces.append(([A + j, B + j, B + i, A + i], 1))   # shaft  (tower)
        faces.append(([B + j, C + j, C + i, B + i], 2))   # lamp-house (lamp)
        faces.append(([C + j, AP, C + i], 1))             # roof   (tower)

    pnts = b"".join(struct.pack(">3f", *v) for v in verts)
    srfs = _lwo_asciiz("tower") + _lwo_asciiz("lamp")
    pols = b""
    for idx, surf in faces:
        pols += struct.pack(">H", len(idx))
        pols += b"".join(struct.pack(">H", v) for v in idx)
        pols += struct.pack(">H", surf)
    surf_tower = _surf_body("tower", tower_col, shaft_lum, 1.0, 0)   # faceted, faint self-lit
    surf_lamp = _surf_body("lamp", lamp_col, lamp_lum, 0.0, 1)       # Surf_Luminous, pure emissive

    body = (_lwo_chunk(b"PNTS", pnts) + _lwo_chunk(b"SRFS", srfs)
            + _lwo_chunk(b"POLS", pols)
            + _lwo_chunk(b"SURF", surf_tower) + _lwo_chunk(b"SURF", surf_lamp))
    data = b"FORM" + struct.pack(">I", len(body) + 4) + b"LWOB" + body
    with open(path, "wb") as fh:
        fh.write(data)
    return lamp_ctr


def lamp_center_from_lwo(path, surfname="lamp"):
    """Parse a LWO1 (FORM..LWOB) and return the local-Y centre of the surface
    named `surfname` (the lamp-house) — the beam origin above the tower base.
    Model-agnostic: works for the procedural beacon.lwo AND the real converted
    lighthouse.lwo, so swapping the mesh needs no hand-measured anchor."""
    d = open(path, "rb").read()
    if d[:4] != b"FORM" or d[8:12] != b"LWOB":
        sys.exit("%s: not a FORM/LWOB LWO" % path)
    off = 12
    end = 8 + struct.unpack(">I", d[4:8])[0]
    pnts = srfs = pols = None
    while off < end:
        cid = d[off:off + 4]
        ln = struct.unpack(">I", d[off + 4:off + 8])[0]
        body = d[off + 8:off + 8 + ln]
        if cid == b"PNTS":
            pnts = body
        elif cid == b"SRFS":
            srfs = body
        elif cid == b"POLS":
            pols = body
        off += 8 + ln + (ln & 1)
    names = [x.decode("latin-1") for x in srfs.split(b"\x00") if x]
    if surfname not in names:
        sys.exit("%s: no '%s' surface (has %s)" % (path, surfname, names))
    si = names.index(surfname) + 1                 # POLS surface is 1-based
    ys = []
    p = 0
    while p + 4 <= len(pols):
        nv = struct.unpack_from(">H", pols, p)[0]; p += 2
        idx = struct.unpack_from(">%dH" % nv, pols, p); p += 2 * nv
        surf = struct.unpack_from(">h", pols, p)[0]; p += 2
        if surf == si:
            for v in idx:
                ys.append(struct.unpack_from(">f", pnts, v * 12 + 4)[0])
    if not ys:
        sys.exit("%s: '%s' surface has no polygons" % (path, surfname))
    return 0.5 * (min(ys) + max(ys))


# ── terrain sampler: mountain height under a world (x,z) ──────────────────────
# The tower BASE must sit under the water / inside the island rock (user note:
# "lighthouses height need to consider the terrain"). Sample the mountain
# meshes' world triangles at the tower footprint and seat the base below the
# LOWEST footprint sample. Transform per instance follows the engine
# (Transform.cpp / chase_sea_level.py): world = pos + s*R(u - pivot), with the
# LightWave rotation order bank(Z) → pitch(X) → heading(Y); heading 0 = +Z,
# H = atan2(x, z) (the aim_hp convention validated by the beams).
MOUNTAIN_BASES = {"m1.lwo", "m2.lwo", "m3.lwo", "m4.lwo", "m5.lwo",
                  "mm7.lwo", "big_m.lwo"}
_mesh_cache = {}


def _load_lwo_tris(path):
    """[(v0,v1,v2)] raw-local triangles (fan-triangulated LWO polys)."""
    if path in _mesh_cache:
        return _mesh_cache[path]
    d = open(path, "rb").read()
    off, end = 12, 8 + struct.unpack(">I", d[4:8])[0]
    pnts = pols = None
    while off < end:
        cid = d[off:off + 4]
        ln = struct.unpack(">I", d[off + 4:off + 8])[0]
        if cid == b"PNTS":
            pnts = d[off + 8:off + 8 + ln]
        elif cid == b"POLS":
            pols = d[off + 8:off + 8 + ln]
        off += 8 + ln + (ln & 1)
    n = len(pnts) // 12
    verts = [struct.unpack_from(">3f", pnts, i * 12) for i in range(n)]
    tris = []
    p = 0
    while p + 4 <= len(pols):
        nv = struct.unpack_from(">H", pols, p)[0]; p += 2
        idx = struct.unpack_from(">%dH" % nv, pols, p); p += 2 * nv + 2
        for i in range(1, nv - 1):
            tris.append((verts[idx[0]], verts[idx[i]], verts[idx[i + 1]]))
    _mesh_cache[path] = tris
    return tris


def _rot_lw(u, h, p, b):
    """R(H,P,B)*u — bank about Z, then pitch about X, then heading about Y."""
    x, y, z = u
    br, pr, hr = math.radians(b), math.radians(p), math.radians(h)
    x, y = x * math.cos(br) - y * math.sin(br), x * math.sin(br) + y * math.cos(br)
    y, z = y * math.cos(pr) - z * math.sin(pr), y * math.sin(pr) + z * math.cos(pr)
    x, z = x * math.cos(hr) + z * math.sin(hr), -x * math.sin(hr) + z * math.cos(hr)
    return x, y, z


def build_terrain(lines):
    """World-space triangle soup of every mountain instance in the LWS."""
    tris = []
    n = len(lines)
    li = 0
    while li < n:
        s = lines[li].strip()
        if s.startswith("LoadObject"):
            base = s[len("LoadObject"):].replace("/", "\\").split("\\")[-1].strip().lower()
            j = li + 1
            mo = None
            pivot = (0.0, 0.0, 0.0)
            while j < n:
                t = lines[j].strip()
                if t.startswith(("LoadObject", "AddLight", "AddNullObject",
                                 "AmbientColor", "ShowCamera")):
                    break
                if t.startswith("ObjectMotion") and mo is None:
                    mo = j
                if t.startswith("PivotPoint"):
                    pv = t.split()[1:]
                    pivot = (float(pv[0]), float(pv[1]), float(pv[2]))
                j += 1
            if base in MOUNTAIN_BASES and mo is not None:
                k = mo + 1
                def nxt(m):
                    while m < n and lines[m].strip() == "": m += 1
                    return m
                k = nxt(k); k = nxt(k + 1); vi = nxt(k + 1)
                v = [float(x) for x in lines[vi].split()]
                pos, rot, scl = v[0:3], v[3:6], v[6:9]
                local = _load_lwo_tris(os.path.join(os.path.dirname(LWS), base))
                for (a, bb, c) in local:
                    w = []
                    for u in (a, bb, c):
                        r = _rot_lw((u[0] - pivot[0], u[1] - pivot[1], u[2] - pivot[2]),
                                    rot[0], rot[1], rot[2])
                        w.append((pos[0] + scl[0] * r[0],
                                  pos[1] + scl[1] * r[1],
                                  pos[2] + scl[2] * r[2]))
                    tris.append(tuple(w))
            li = j
            continue
        li += 1
    return tris


def terrain_height(tris, x, z):
    """Max triangle-surface Y at world (x,z); 0.0 (the water plane) if none."""
    best = 0.0
    for (a, b, c) in tris:
        if x < min(a[0], b[0], c[0]) or x > max(a[0], b[0], c[0]):
            continue
        if z < min(a[2], b[2], c[2]) or z > max(a[2], b[2], c[2]):
            continue
        d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if abs(d) < 1e-9:
            continue
        w0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
        w1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
        w2 = 1.0 - w0 - w1
        if w0 < -1e-6 or w1 < -1e-6 or w2 < -1e-6:
            continue
        y = w0 * a[1] + w1 * b[1] + w2 * c[1]
        if y > best:
            best = y
    return best


def footprint_terrain(tris, x, z, radius):
    """(min, max) terrain over the tower footprint (centre + 8 ring samples)."""
    hs = [terrain_height(tris, x, z)]
    for k in range(8):
        a = 2.0 * math.pi * k / 8
        hs.append(terrain_height(tris, x + radius * math.cos(a),
                                 z + radius * math.sin(a)))
    return min(hs), max(hs)


# ── runway: a colonnade of ANCHORED, slowly-SWEEPING lighthouse beacons ────────
# Auto-placed from Ship1's ground track: N beacons (default 8, modest — the +63
# corridor once broke ship2's key count, 2969679) alternate L/R down the OPEN-
# water lane, skipping the mm7 gorge band so no tower buries in the canyon. Each
# beacon = a static beacon.lwo tower (LoadObject) + a spinning null rotor
# (AddNullObject) + a LightType-2 volumetric spot PARENTED to the rotor. The
# parented spot's cone axis IDir is re-derived every frame from the rotor's
# heading (Transform.cpp:330), so the beam SWEEPS; a non-parented spot's IDir is
# frozen at load. Phases are staggered along the lane so the sweeps travel down
# the corridor rather than strobing in unison. LOW gain by design (atmosphere,
# not a stage show — the user vetoed bright/messy).
#
# Returns (obj_block, lgt_block, new_obj_index). Object indices are 1-based in
# LWS load order; the first appended object is start_idx+1 (the proven
# ParentObject convention — city headlights / the --lighthouse accent).
OPEN_BANDS = [(160.0, 980.0), (1275.0, 1610.0)]   # lane minus the gorge ~[1000,1250]


def _beacon_frames(n, bands):
    """n frames spread evenly across the concatenated open bands (centred)."""
    total = sum(b1 - b0 for b0, b1 in bands)
    out = []
    for i in range(n):
        t = (i + 0.5) / n * total
        acc = 0.0
        placed = False
        for b0, b1 in bands:
            L = b1 - b0
            if t <= acc + L:
                out.append(b0 + (t - acc)); placed = True; break
            acc += L
        if not placed:
            out.append(bands[-1][1])
    return out


def _beacon_rotor(idx, pos, pitch, turns, phase):
    """Object block: a null at the lamp, heading = phase + turns*360 over the
    scene (turns=0 → a static aim), pitch = fixed downward rake. The parented
    spot rides this heading, so the beam sweeps (or holds)."""
    NKEYS, LAST = 9, 1760
    out = ["AddNullObject fds_beacon_rotor_%d" % idx,
           "ShowObject 8 7", "ObjectMotion (unnamed)", "  9", "  %d" % NKEYS]
    for k in range(NKEYS):
        f = int(round(LAST * k / (NKEYS - 1)))
        H = phase + turns * 360.0 * k / (NKEYS - 1)
        out.append("  %s %s %s %s %s 0 1 1 1"
                   % (fmt(pos[0]), fmt(pos[1]), fmt(pos[2]), fmt(H), fmt(pitch)))
        out.append("  %d 0 0 0 0" % f)
    out += ["EndBehavior 1", "LockedChannels 8", "ShadowOptions 7"]
    return out


# Colour palette for the beacon beams (user: "colored spotlights, with dynamic
# changes"). Each beacon gets a PAIR (i, i+3): two co-located spots on the same
# rotor whose LgtIntensity ENVELOPES crossfade in anti-phase, so the beam (and
# its water pool) continuously shifts colour. The engine re-evaluates the
# intensity spline every frame (Transform.cpp:255 Spline_Calc_1D → ISize) and
# the light SoA colour is L * ISize (DeferredSurfaceKernel.cpp:5328), so the
# authored envelope IS the dynamic change — no engine work.
BEAM_PALETTE = [
    (255,  40,  40),   # red
    ( 40, 170, 255),   # cyan-blue
    (255, 160,  40),   # amber
    ( 60, 255, 110),   # green
    (255,  70, 220),   # magenta
    ( 90, 110, 255),   # indigo
]
SCENE_LAST = 1760


def intensity_env_lines(hi, lo, period, phase, step):
    """'LgtIntensity  (envelope)' block spanning the whole scene (no repeat —
    ConvertOmni honours EndBehavior only for Pos, so the keys must cover
    0..1760 explicitly). TWO spaces before '(' — ReadEnvelope detects an
    envelope by Temp[1]=='(' after the keyword's first space.
    Curve = HALF-WAVE-RECTIFIED cosine: the light holds near `lo` for half the
    period and peaks at `hi` in the other half. Paired with an anti-phase twin
    this makes the beam SWITCH colour with brief blends, instead of spending
    half the cycle in a desaturated 50/50 mix (a plain cos crossfade reads
    grey-ish through the white fog in-scatter)."""
    out = ["LgtIntensity  (envelope)", "  1"]
    frames = list(range(0, SCENE_LAST + 1, step))
    if frames[-1] != SCENE_LAST:
        frames.append(SCENE_LAST)
    out.append("  %d" % len(frames))
    for f in frames:
        c = math.cos(2.0 * math.pi * (f - phase) / period)
        v = lo + (hi - lo) * (max(0.0, c) ** 1.2)
        out.append("  %s" % fmt(max(lo * 0.5, v)))
        out.append("  %d 0 0 0 0" % f)
    out.append("EndBehavior 1")
    return out


def _beacon_spot(name, rotor_num, color, cone, gain, env_lines):
    return (["AddLight",
             "LightName %s" % name,
             "ShowLight 1 7",
             "LightMotion (unnamed)",
             "  9",
             "  1",
             "  0 0 0 0 0 0 1 1 1",
             "  0 0 0 0 0",
             "EndBehavior 1",
             "ParentObject %d" % rotor_num,
             "LightColor %d %d %d" % (color[0], color[1], color[2])]
            + env_lines
            + ["LightType 2",
               "ConeAngle %f" % cone,
               "LightRange 30000.000000",
               "VolumetricLight 1",
               "VolumetricLightIntensity %f" % gain,
               "ShadowType 1"])


def build_runway(lines, args, start_idx, model_base, lamp_local):
    keys = parse_ship_keys(lines, "ship1.lwo")
    n = max(1, args.runway_beacons)
    offset = args.runway_offset
    # Auto-scale the model so its lamp-house lands --runway-lamp-height above
    # the WATER for a sea-level tower, regardless of the mesh's native size —
    # a downloaded model of any scale slots in. ONE scale for all beacons
    # (uniform architecture); island towers simply ride higher on the rock.
    # --runway-scale > 0 forces a fixed scale instead. --runway-stock widens
    # X/Z only (stockier silhouette vs the slender source mesh).
    if args.runway_scale > 0:
        scale = args.runway_scale
    else:
        scale = (args.runway_lamp_height + args.runway_sink) / (lamp_local or 1.0)
    stock = args.runway_stock
    lamp_y_local = lamp_local * scale
    # world footprint radius (source footprint ~6 units across → r≈3.2 local)
    foot_r = 3.2 * scale * stock
    gain, cone = args.runway_gain, args.runway_cone
    pitch = args.runway_pitch
    turns = args.runway_turns
    terrain = build_terrain(lines)
    frames = _beacon_frames(n, OPEN_BANDS)
    obj_block, lgt_block = [], []
    idx = start_idx
    for i, f in enumerate(frames):
        p = path_at(keys, f)
        tx, tz = path_tangent_xz(keys, f)
        lx, lz = tz, -tx                    # right-hand lateral (horizontal)
        side = +1.0 if (i % 2 == 0) else -1.0
        ex = p[0] + side * lx * offset
        ez = p[2] + side * lz * offset
        # Terrain-aware seating: the base must sit under the water / inside
        # the island rock. Sample the mountains over the footprint and put the
        # base below the LOWEST sample; sink deeper on rock so the base bevel
        # is fully buried in the slope.
        t_min, t_max = footprint_terrain(terrain, ex, ez, foot_r)
        on_rock = t_max > 5.0
        sink = args.runway_rock_sink if on_rock else args.runway_sink
        base_y = t_min - sink
        lamp_world_y = base_y + lamp_y_local
        print("  beacon %d @f%-4d (%.0f, %.0f)  terrain %.0f..%.0f %s base=%.0f lamp=%.0f"
              % (i, f, ex, ez, t_min, t_max, "ROCK " if on_rock else "water",
                 base_y, lamp_world_y))
        # this beacon rotates unless --runway-fixed-alt makes the odd ones hold
        rotating = not (args.runway_fixed_alt and (i % 2 == 1))
        if rotating:
            b_turns = turns
            b_phase = i * args.runway_phase
        else:
            b_turns = 0.0                   # static: aim its cone across the lane
            aim = (p[0] + tx * args.runway_ahead, 0.0, p[2] + tz * args.runway_ahead)
            b_phase, _pfree = aim_hp((ex, lamp_world_y, ez), aim)
        # tower (static) — a real structure the beam springs from
        obj_block += [
            "LoadObject %s" % model_base,
            "ShowObject 8 7",
            "ObjectMotion (unnamed)",
            "  9",
            "  1",
            "  %s %s %s 0 0 0 %s %s %s" % (fmt(ex), fmt(base_y), fmt(ez),
                                           fmt(scale * stock), fmt(scale),
                                           fmt(scale * stock)),
            "  0 0 0 0 0",
            "EndBehavior 1",
            "ShadowOptions 7",
        ]
        idx += 1                            # tower object index (unused as parent)
        # rotor null at the lamp
        obj_block += _beacon_rotor(i, (ex, lamp_world_y, ez), pitch, b_turns, b_phase)
        idx += 1
        rotor_num = idx                     # 1-based index of the rotor just added
        # Colored spot(s) parented to the rotor. Dual mode (default): two
        # anti-phase envelope spots crossfade the beam colour; single mode:
        # one colour with a breathing envelope.
        colA = BEAM_PALETTE[i % len(BEAM_PALETTE)]
        colB = BEAM_PALETTE[(i + 3) % len(BEAM_PALETTE)]
        period = args.runway_pulse
        step = max(20, period // 8)
        e_phase = i * period / max(1, n) * 1.7   # stagger the crossfades
        hi, lo = args.runway_int_hi, args.runway_int_lo
        if args.runway_dual:
            envA = intensity_env_lines(hi, lo, period, e_phase, step)
            envB = intensity_env_lines(hi, lo, period, e_phase + period * 0.5, step)
            lgt_block += _beacon_spot("fds runway %dA" % i, rotor_num, colA,
                                      cone, gain, envA)
            lgt_block += _beacon_spot("fds runway %dB" % i, rotor_num, colB,
                                      cone, gain, envB)
        else:
            envA = intensity_env_lines(hi, max(lo, 0.35 * hi), period, e_phase, step)
            lgt_block += _beacon_spot("fds runway %dA" % i, rotor_num, colA,
                                      cone, gain, envA)
    return obj_block, lgt_block, idx


# ── lighthouse: a spinning null (object) + a parented volumetric spot (light) ──
def build_lighthouse_null(objnum_unused, pos, pitch, turns):
    """Object-section block: a null spinning its heading `turns` times over the
    scene, held at a constant downward `pitch`, so a parented spot's beam rakes a
    cone around the vertical."""
    NKEYS = 9
    LAST = 1760
    lines = [
        "AddNullObject fds_lighthouse_rotor",
        "ShowObject 8 7",
        "ObjectMotion (unnamed)",
        "  9",
        "  %d" % NKEYS,
    ]
    for i in range(NKEYS):
        f = int(round(LAST * i / (NKEYS - 1)))
        H = turns * 360.0 * i / (NKEYS - 1)
        lines.append("  %s %s %s %s %s 0 1 1 1"
                     % (fmt(pos[0]), fmt(pos[1]), fmt(pos[2]), fmt(H), fmt(pitch)))
        lines.append("  %d 0 0 0 0" % f)
    lines += ["EndBehavior 1", "LockedChannels 8", "ShadowOptions 7"]
    return lines


def build_lighthouse_light(parent_num, gain, cone, color):
    return [
        "AddLight",
        "LightName fds lighthouse beam",
        "ShowLight 1 7",
        "LightMotion (unnamed)",
        "  9",
        "  1",
        "  0 0 0 0 0 0 1 1 1",
        "  0 0 0 0 0",
        "EndBehavior 1",
        "ParentObject %d" % parent_num,
        "LightColor %d %d %d" % (color[0], color[1], color[2]),
        "LgtIntensity 2.400000",
        "LightType 2",
        "ConeAngle %f" % cone,
        "LightRange 60000.000000",
        "VolumetricLight 1",
        "VolumetricLightIntensity %f" % gain,
        "ShadowType 1",
    ]


# ── laser gauntlet: static non-parented volumetric spots crossing the passage ──
# Placed on ALTERNATING sides of the ship path through the open-island stretch
# (frames ~1200-1360), each aimed at a point low on the OPPOSITE side so the
# thin shafts crisscross the corridor the ships thread. Saturated colours.
LASER_COLORS = [
    (255,  40,  90),   # hot pink
    ( 60, 200, 255),   # cyan
    (120, 255,  80),   # laser green
    (255, 150,  30),   # amber
    (180,  80, 255),   # violet
    (255,  70,  40),   # red-orange
]
# path anchors (X,Y,Z) roughly following ship1 through the island stretch
LASER_PATH = [
    (-14000, 700, -6000),
    (-17000, 720,  -500),
    (-20000, 720,  4500),
    (-23000, 620,  9500),
    (-26000, 430, 14500),
    (-29000, 360, 19000),
]


def build_lasers(count, gain, cone):
    out = []
    n = max(1, min(count, len(LASER_PATH) * 2))
    # path travel dir (XZ) ~ (-0.51, 0, 0.86); lateral "right" perp = (0.86,0,0.51)
    LAT = (0.86, 0.0, 0.51)
    for k in range(n):
        anchor = LASER_PATH[(k // 2) % len(LASER_PATH)]
        side = +1.0 if (k % 2 == 0) else -1.0     # alternate emitter sides
        # emitter: out to `side`, moderately elevated so the shaft crosses the
        # corridor at flight altitude rather than washing the sky.
        emit = (anchor[0] + LAT[0] * side * 5000,
                anchor[1] + 1700,
                anchor[2] + LAT[2] * side * 5000)
        # aim LOW across to the FAR side → a thin bar crossing the flight path.
        tgt = (anchor[0] - LAT[0] * side * 3000,
               120.0,
               anchor[2] - LAT[2] * side * 3000)
        H, P = aim_hp(emit, tgt)
        col = LASER_COLORS[k % len(LASER_COLORS)]
        out += [
            "AddLight",
            "LightName fds laser %d" % k,
            "ShowLight 1 7",
            "LightMotion (unnamed)",
            "  9",
            "  1",
            "  %s %s %s %s %s 0 1 1 1" % (fmt(emit[0]), fmt(emit[1]), fmt(emit[2]),
                                          fmt(H), fmt(P)),
            "  0 0 0 0 0",
            "EndBehavior 1",
            "LightColor %d %d %d" % col,
            "LgtIntensity 1.600000",
            "LightType 2",
            "ConeAngle %f" % cone,
            "LightRange 40000.000000",
            "VolumetricLight 1",
            "VolumetricLightIntensity %f" % gain,
            "ShadowType 1",
        ]
    return out


# ── realign the two EXISTING canyon spots so they actually rake the path ───────
# Recompute position + aim from fixed targets each run (idempotent). These are
# the L2.3 spots SESSION_STATE flags as invisible (aimed flat from Y=4000).
REALIGN = {
    "canyon warm spot": dict(pos=(-16000, 3000, 2000), tgt=(-20000, 200, 4000),
                             gain=22.0, cone=22.0),
    "canyon cool spot": dict(pos=(-30000, 3200, 16000), tgt=(-26000, 200, 12000),
                             gain=22.0, cone=22.0),
}


def realign_spots(lines):
    n = len(lines)
    changed = 0
    i = 0
    while i < n:
        if lines[i].strip().startswith("LightName"):
            name = lines[i].strip()[len("LightName"):].strip()
            if name in REALIGN:
                cfg = REALIGN[name]
                H, P = aim_hp(cfg["pos"], cfg["tgt"])
                # find the single motion key value line (2 lines after LightMotion)
                j = i
                while j < n and not lines[j].strip().startswith("LightMotion"):
                    j += 1
                # value line = skip channels + nkeys
                k = j + 1
                while k < n and lines[k].strip() == "":
                    k += 1
                k += 1                            # channels line
                while k < n and lines[k].strip() == "":
                    k += 1
                k += 1                            # nkeys line
                while k < n and lines[k].strip() == "":
                    k += 1
                p = cfg["pos"]
                lines[k] = "  %s %s %s %s %s 0 1 1 1" % (
                    fmt(p[0]), fmt(p[1]), fmt(p[2]), fmt(H), fmt(P))
                # rewrite gain + cone in the block below
                m = k
                while m < n and not lines[m].strip().startswith("AddLight") \
                        and not lines[m].strip().startswith("ShowCamera"):
                    s = lines[m].strip()
                    if s.startswith("VolumetricLightIntensity"):
                        lines[m] = "VolumetricLightIntensity %f" % cfg["gain"]
                    elif s.startswith("ConeAngle"):
                        lines[m] = "ConeAngle %f" % cfg["cone"]
                    elif s.startswith("LgtIntensity"):
                        lines[m] = "LgtIntensity 2.400000"
                    m += 1
                changed += 1
        i += 1
    return changed


def insert_before(lines, needle, block):
    """Insert block (list) right before the first line whose strip startswith needle."""
    for i, ln in enumerate(lines):
        if ln.strip().startswith(needle):
            return lines[:i] + block + lines[i:]
    sys.exit("insertion anchor %r not found" % needle)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runway", action="store_true",
                    help="PRIMARY: dim cool spot colonnade lining the flight lane")
    ap.add_argument("--lighthouse", action="store_true",
                    help="secondary accent: a slow rotating warm beam")
    ap.add_argument("--lasers", type=int, default=0, metavar="N",
                    help="legacy sky-fan lasers (tamed; off by default)")
    ap.add_argument("--realign", action="store_true")
    ap.add_argument("--clear", action="store_true")
    # runway knobs — LOW gain by design (atmosphere, not stage show).
    # Beacons are ANCHORED (each beam springs from a beacon.lwo tower) and SWEEP
    # (spot parented to a spinning rotor null). Count is MODEST — +63 objects
    # once miscounted ship2's keys (2969679); 8 taller beacons, not 16 pairs.
    ap.add_argument("--runway-beacons", "--runway-pairs", dest="runway_beacons",
                    type=int, default=8,
                    help="number of anchored beacons along the lane (modest — keep <=~10)")
    ap.add_argument("--runway-gain", type=float, default=7.0,
                    help="VolumetricLightIntensity — keep low; the beams are ambience")
    ap.add_argument("--runway-cone", type=float, default=9.0)
    ap.add_argument("--runway-offset", type=float, default=2300.0,
                    help="lateral distance of each beacon from the lane centreline")
    ap.add_argument("--runway-ahead", type=float, default=2400.0,
                    help="fixed beacons: how far along the path the cone aims (rake)")
    # colour + dynamics (user: "colored spotlights, with dynamic changes").
    # Colours come from BEAM_PALETTE per beacon; dual anti-phase intensity
    # envelopes crossfade each beam between its colour pair.
    ap.add_argument("--runway-dual", type=int, default=1,
                    help="1 = two crossfading colour spots per beacon (default), 0 = single pulsing")
    ap.add_argument("--runway-pulse", type=int, default=440,
                    help="colour-crossfade / pulse period in frames")
    ap.add_argument("--runway-int-hi", type=float, default=1.6,
                    help="LgtIntensity envelope peak")
    ap.add_argument("--runway-int-lo", type=float, default=0.2,
                    help="LgtIntensity envelope floor")
    # rotation — SLOW + staggered so sweeps travel down the corridor (user asked
    # for rotation; the user vetoed bright/messy, so keep it stately).
    ap.add_argument("--runway-turns", type=float, default=3.0,
                    help="full beam revolutions over the whole scene (0 = static aim)")
    ap.add_argument("--runway-phase", type=float, default=47.0,
                    help="per-beacon start-heading offset (deg) so sweeps travel, not strobe")
    ap.add_argument("--runway-pitch", type=float, default=48.0,
                    help="downward beam rake (deg below horizontal); the cone sweeps a shallow cone")
    ap.add_argument("--runway-fixed-alt", action="store_true",
                    help="alternate (odd) beacons hold a static aim (calmer); default = all sweep")
    # model + placement. Default = the real CC0 lighthouse (Authoring/chase/
    # lighthouse.lwo, converted from OBJ via tools/obj2lwo.py). --runway-model
    # proc regenerates + uses the procedural faceted tower (beacon.lwo) instead.
    ap.add_argument("--runway-model", choices=["lighthouse", "proc"], default="lighthouse",
                    help="beacon mesh: the real CC0 lighthouse (default) or the procedural tower")
    ap.add_argument("--runway-lamp-height", type=float, default=1400.0,
                    help="world-Y the lamp-house (beam origin) is auto-scaled to sit at")
    ap.add_argument("--runway-scale", type=float, default=0.0,
                    help="force a fixed uniform tower scale (0 = auto from --runway-lamp-height)")
    ap.add_argument("--runway-stock", type=float, default=1.35,
                    help="X/Z-only widening of the tower (stockier than the slender source mesh)")
    ap.add_argument("--runway-sink", type=float, default=35.0,
                    help="how far the tower base sits under the WATER plane (no z-fight)")
    ap.add_argument("--runway-rock-sink", type=float, default=90.0,
                    help="deeper sink when the footprint lands on island rock (bury the base bevel)")
    # procedural-tower geometry (only used with --runway-model proc; real world
    # units, sea level = Y 0, ship hull radius ~175-264)
    ap.add_argument("--runway-tower-height", type=float, default=1400.0)
    ap.add_argument("--runway-base-r", type=float, default=340.0)
    ap.add_argument("--runway-top-r", type=float, default=150.0)
    ap.add_argument("--runway-lamp-r", type=float, default=235.0)
    ap.add_argument("--runway-tower-color", type=int, nargs=3, default=[118, 124, 145])
    ap.add_argument("--runway-lamp-color", type=int, nargs=3, default=[188, 212, 240])
    ap.add_argument("--runway-lamp-lum", type=float, default=0.016,
                    help="proc lamp emissive fraction (VLUM; legacy Luminosity = x100)")
    ap.add_argument("--runway-shaft-lum", type=float, default=0.004,
                    help="proc faint shaft self-emissive so the tower is never pure-black at night")
    # lighthouse — retuned WAY down (secondary accent, not the show)
    ap.add_argument("--lh-pos", type=float, nargs=3, default=[-15000, 2600, 8000])
    ap.add_argument("--lh-pitch", type=float, default=40.0)
    ap.add_argument("--lh-turns", type=float, default=8.5)
    ap.add_argument("--lh-gain", type=float, default=8.0)
    ap.add_argument("--lh-cone", type=float, default=14.0)
    ap.add_argument("--lh-color", type=int, nargs=3, default=[255, 206, 128])
    # lasers — tamed hard (legacy; not in the default variant set)
    ap.add_argument("--laser-gain", type=float, default=7.0)
    ap.add_argument("--laser-cone", type=float, default=6.0)
    args = ap.parse_args()

    lines = open(LWS, encoding="latin-1").read().split("\n")
    # always strip prior managed regions first (idempotence)
    lines = strip_region(lines, OBJ_BEGIN, OBJ_END)
    lines = strip_region(lines, LGT_BEGIN, LGT_END)

    if args.realign:
        c = realign_spots(lines)
        print("realigned %d existing canyon spot(s)" % c)

    if args.clear and not (args.lighthouse or args.lasers or args.runway):
        open(LWS, "w", encoding="latin-1").write("\n".join(lines))
        print("cleared all fds lights")
        return

    base_objs = count_objects(lines)
    obj_idx = base_objs                 # 1-based index of the last existing object

    obj_block, lgt_block = [], []
    if args.runway:
        # Pick the beacon mesh. The LoadObject lines reference it by basename;
        # lwsread runs from Authoring/chase, so it resolves there. The beam
        # origin (lamp-house centre local-Y) is READ BACK from the mesh, so a
        # swapped-in model needs no hand-measured anchor.
        if args.runway_model == "proc":
            write_beacon_lwo(
                BEACON_LWO, args.runway_tower_height, args.runway_base_r,
                args.runway_top_r, args.runway_lamp_r,
                args.runway_tower_color, args.runway_lamp_color,
                args.runway_lamp_lum, args.runway_shaft_lum)
            model_base = "beacon.lwo"
            lamp_local = lamp_center_from_lwo(BEACON_LWO)
        else:
            if not os.path.exists(LIGHTHOUSE_LWO):
                sys.exit("missing %s — convert it first:\n  tools/obj2lwo.py <lighthouse.obj> "
                         "%s --recenter --map ..." % (LIGHTHOUSE_LWO, LIGHTHOUSE_LWO))
            model_base = "lighthouse.lwo"
            lamp_local = lamp_center_from_lwo(LIGHTHOUSE_LWO)
        r_obj, r_lgt, obj_idx = build_runway(lines, args, obj_idx, model_base, lamp_local)
        obj_block += r_obj
        lgt_block += r_lgt
    if args.lighthouse:
        rotor_num = obj_idx + 1
        obj_block += build_lighthouse_null(rotor_num, args.lh_pos,
                                           args.lh_pitch, args.lh_turns)
        obj_idx += 1
        lgt_block += build_lighthouse_light(rotor_num, args.lh_gain,
                                            args.lh_cone, args.lh_color)
    if args.lasers > 0:
        lgt_block += build_lasers(args.lasers, args.laser_gain, args.laser_cone)

    # NB: no surrounding blank lines added — the pre-existing blank before each
    # anchor is preserved, so strip_region returns the file EXACTLY to base
    # (idempotent re-runs; --clear reverts byte-clean).
    if obj_block:
        lines = insert_before(lines, "AmbientColor",
                              [OBJ_BEGIN] + obj_block + [OBJ_END])
    if lgt_block:
        lines = insert_before(lines, "ShowCamera",
                              [LGT_BEGIN] + lgt_block + [LGT_END])

    open(LWS, "w", encoding="latin-1").write("\n".join(lines))
    print("wrote lights: runway=%s(%d beacons) lighthouse=%s lasers=%d realign=%s (base objs=%d, last idx=%d)"
          % (args.runway, args.runway_beacons if args.runway else 0,
             args.lighthouse, args.lasers, args.realign, base_objs, obj_idx))
    print("NOW: regen (lwsread_legacy) + install")


if __name__ == "__main__":
    main()
