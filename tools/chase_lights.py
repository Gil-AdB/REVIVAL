#!/usr/bin/env python3
"""Author ENVIRONMENTAL-LIGHT DRAMA into Authoring/chase/CHASE.LWS.

PRIMARY variant --runway: a colonnade of ANCHORED, slowly-SWEEPING lighthouse
beacons lining the flight lane (auto-placed from Ship1's ground track, alternating
sides, in the open-water stretches). Each beam springs from a REAL structure — a
faceted beacon.lwo tower standing in the water with a luminous lamp-house cap —
and the beam SWEEPS, because its spot is parented to a spinning null rotor
(phases staggered so the sweeps travel down the corridor, not strobe in unison).
LOW gain by design: atmosphere, not a stage show. This answers the user note
"the spot lights are not anchored to anything - there should be at least a
lighthouse model or something, and they should rotate?". Secondary --lighthouse
(a slow rotating warm accent, retuned WAY down). --lasers is the legacy sky-fan
gauntlet, tamed hard and no longer in the default variant set. --realign re-aims
the two existing canyon spots. --runway (re)writes Authoring/chase/beacon.lwo.

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
                     --runway-offset U  --runway-color R G B
                     --runway-turns T  --runway-phase DEG  --runway-pitch DEG
                     --runway-fixed-alt  --runway-tower-height U
                     --runway-base-r/-top-r/-lamp-r U  --runway-lamp-lum F
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


def build_runway(lines, args, start_idx, model_base, lamp_local):
    keys = parse_ship_keys(lines, "ship1.lwo")
    n = max(1, args.runway_beacons)
    offset = args.runway_offset
    place_y = -args.runway_sink            # tower base sits just under the water
    # Auto-scale the model so its lamp-house lands at --runway-lamp-height (the
    # beam origin), regardless of the mesh's native size — a downloaded model of
    # any scale slots in. --runway-scale > 0 forces a fixed uniform scale instead.
    if args.runway_scale > 0:
        scale = args.runway_scale
    else:
        scale = (args.runway_lamp_height - place_y) / (lamp_local or 1.0)
    lamp_y_local = lamp_local * scale
    gain, cone = args.runway_gain, args.runway_cone
    color = args.runway_color
    pitch = args.runway_pitch
    turns = args.runway_turns
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
        lamp_world_y = place_y + lamp_y_local
        # this beacon rotates unless --runway-fixed-alt makes the odd ones hold
        rotating = not (args.runway_fixed_alt and (i % 2 == 1))
        if rotating:
            b_turns = turns
            b_phase = i * args.runway_phase
        else:
            b_turns = 0.0                   # static: aim its cone across the lane
            aim = (p[0] + tx * args.runway_ahead, place_y, p[2] + tz * args.runway_ahead)
            b_phase, _pfree = aim_hp((ex, lamp_world_y, ez), aim)
        # tower (static) — a real structure the beam springs from
        obj_block += [
            "LoadObject %s" % model_base,
            "ShowObject 8 7",
            "ObjectMotion (unnamed)",
            "  9",
            "  1",
            "  %s %s %s 0 0 0 %s %s %s" % (fmt(ex), fmt(place_y), fmt(ez),
                                           fmt(scale), fmt(scale), fmt(scale)),
            "  0 0 0 0 0",
            "EndBehavior 1",
            "ShadowOptions 7",
        ]
        idx += 1                            # tower object index (unused as parent)
        # rotor null at the lamp
        obj_block += _beacon_rotor(i, (ex, lamp_world_y, ez), pitch, b_turns, b_phase)
        idx += 1
        rotor_num = idx                     # 1-based index of the rotor just added
        # spot parented to the rotor → beam emanates from the lamp and sweeps
        lgt_block += [
            "AddLight",
            "LightName fds runway %d%s" % (i, "R" if side > 0 else "L"),
            "ShowLight 1 7",
            "LightMotion (unnamed)",
            "  9",
            "  1",
            "  0 0 0 0 0 0 1 1 1",
            "  0 0 0 0 0",
            "EndBehavior 1",
            "ParentObject %d" % rotor_num,
            "LightColor %d %d %d" % (color[0], color[1], color[2]),
            "LgtIntensity 1.600000",
            "LightType 2",
            "ConeAngle %f" % cone,
            "LightRange 30000.000000",
            "VolumetricLight 1",
            "VolumetricLightIntensity %f" % gain,
            "ShadowType 1",
        ]
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
    ap.add_argument("--runway-gain", type=float, default=3.4,
                    help="VolumetricLightIntensity — keep low; the beams are ambience")
    ap.add_argument("--runway-cone", type=float, default=9.0)
    ap.add_argument("--runway-offset", type=float, default=2300.0,
                    help="lateral distance of each beacon from the lane centreline")
    ap.add_argument("--runway-ahead", type=float, default=2400.0,
                    help="fixed beacons: how far along the path the cone aims (rake)")
    ap.add_argument("--runway-color", type=int, nargs=3, default=[135, 185, 230],
                    help="cool blue-white by default")
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
    ap.add_argument("--runway-sink", type=float, default=35.0,
                    help="how far the tower base sits under the water plane (no z-fight)")
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
