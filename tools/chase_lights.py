#!/usr/bin/env python3
"""Author ENVIRONMENTAL-LIGHT DRAMA into Authoring/chase/CHASE.LWS: a rotating
lighthouse beam and/or a gauntlet of crisscrossing laser beams along the
passage, plus a re-aim of the two existing (invisible) canyon spots.

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
  tools/chase_lights.py [--lighthouse] [--lasers N] [--realign] [--clear]
  tools/chase_lights.py --lighthouse                 # rotating beam only
  tools/chase_lights.py --lasers 8                   # laser gauntlet only
  tools/chase_lights.py --lighthouse --lasers 8 --realign   # the works
  tools/chase_lights.py --clear                      # strip all fds lights

  Lighthouse knobs:  --lh-pos X Y Z  --lh-pitch DEG  --lh-turns T
                     --lh-gain G  --lh-cone DEG  --lh-color R G B
  Laser knobs:       --laser-gain G  --laser-cone DEG
Then: regen (lwsread_legacy) + install (see Authoring/chase/README.md).
"""
import os, sys, math, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS  = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")

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
        "LgtIntensity 6.000000",
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
            "LgtIntensity 4.000000",
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
    ap.add_argument("--lighthouse", action="store_true")
    ap.add_argument("--lasers", type=int, default=0, metavar="N")
    ap.add_argument("--realign", action="store_true")
    ap.add_argument("--clear", action="store_true")
    ap.add_argument("--lh-pos", type=float, nargs=3, default=[-15000, 2600, 8000])
    ap.add_argument("--lh-pitch", type=float, default=40.0)
    ap.add_argument("--lh-turns", type=float, default=8.5)
    ap.add_argument("--lh-gain", type=float, default=32.0)
    ap.add_argument("--lh-cone", type=float, default=15.0)
    ap.add_argument("--lh-color", type=int, nargs=3, default=[255, 206, 128])
    ap.add_argument("--laser-gain", type=float, default=20.0)
    ap.add_argument("--laser-cone", type=float, default=6.0)
    args = ap.parse_args()

    lines = open(LWS, encoding="latin-1").read().split("\n")
    # always strip prior managed regions first (idempotence)
    lines = strip_region(lines, OBJ_BEGIN, OBJ_END)
    lines = strip_region(lines, LGT_BEGIN, LGT_END)

    if args.realign:
        c = realign_spots(lines)
        print("realigned %d existing canyon spot(s)" % c)

    if args.clear and not (args.lighthouse or args.lasers):
        open(LWS, "w", encoding="latin-1").write("\n".join(lines))
        print("cleared all fds lights")
        return

    base_objs = count_objects(lines)

    obj_block, lgt_block = [], []
    if args.lighthouse:
        rotor_num = base_objs + 1
        obj_block += build_lighthouse_null(rotor_num, args.lh_pos,
                                           args.lh_pitch, args.lh_turns)
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
    print("wrote lights: lighthouse=%s lasers=%d realign=%s (base objs=%d)"
          % (args.lighthouse, args.lasers, args.realign, base_objs))
    print("NOW: regen (lwsread_legacy) + install")


if __name__ == "__main__":
    main()
