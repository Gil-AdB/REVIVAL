#!/usr/bin/env python3
"""Add mountains to Authoring/chase/CHASE.LWS to build a complex WINDING canyon
corridor through the gorge stretch (default frames ~990-1360).

Family: tools/chase_bank.py / chase_sea_level.py / chase_camera.py — in-place,
idempotent, parameterized, re-runnable. Authored-first: the result is new
mountain LoadObject instances in the LWS, editor-visible after lwsread_legacy
regen. Run tools/chase_sea_level.py AFTER this to drop the new instances onto
the water plane (they are emitted at Y=0; sea_level computes the real Y), then
tools/chase_camera.py so the trailing camera follows the (possibly re-woven) path.

Design: the two ships thread a corridor along Ship1's path. We line that path
with STAGGERED WALLS on both sides (a serpentine chokepoint pattern — as one
side narrows the other widens, so the open channel snakes) plus occasional
SPIRES the ships weave past. Every instance is placed by CLEARANCE from the
ship centreline: its bounding radius (measured from the LWO, scaled) is added to
the desired corridor half-width so the near face sits exactly `clearance` from
the path. A final safety pass pushes any instance outward until its face clears
BOTH ships' paths by >= --min-face (so we never bury the flight lane).

New instances are appended AFTER ship2 (object 80) so Ship1/ship2 keep indices
79/80 (TargetObject 79 + the parented engine glows stay valid). The block is
wrapped in FDSCORRIDORBEGIN/END marker lines (unknown keywords lwsread skips),
so a re-run strips the old block first — fully idempotent. --count 0 removes it.

Idempotence / determinism: instance positions/types/scales are a pure function
of the ship path + the params (deterministic hash on sample index), never of the
current LWS. Re-run reproduces exactly.

Usage:
    tools/chase_corridor.py [--f0 F --f1 F --step N]
                            [--base-clear U --amp-clear U --wavelen F]
                            [--min-face U] [--seed S] [--dry-run]
    --f0/--f1   gorge frame window to line with walls (default 990..1360).
    --step      frames between wall sample points (default 15).
    --base-clear/--amp-clear  corridor half-width mean/serpentine amplitude
                (default 2150 / 780 → walls 1370..2930 from the centreline).
    --wavelen   serpentine wavelength in frames (default 140).
    --min-face  hard minimum near-face clearance from EITHER ship path (1250).
    --dry-run   print the plan; write nothing.
"""
import os, sys, math, struct, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS  = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")
LWO_DIR = os.path.join(REPO, "Authoring", "chase")

BEGIN = "FDSCORRIDORBEGIN"
END   = "FDSCORRIDOREND"
FIRST_FRAME, LAST_FRAME = 0, 1760


def fmt(v):
    if abs(v) < 1e-9:
        return "0"
    s = "%.4f" % v
    return s.rstrip("0").rstrip(".")


def basename(p):
    return p.replace("/", "\\").split("\\")[-1].strip().lower()


# ── LWO PNTS bounding radius about the object ORIGIN (what LoadObject places) ──
_rcache = {}
def lwo_radius_xz(name):
    if name in _rcache:
        return _rcache[name]
    d = open(os.path.join(LWO_DIR, name), "rb").read()
    p = 12; verts = None
    while p + 8 <= len(d):
        tag = d[p:p+4]; ln = struct.unpack(">I", d[p+4:p+8])[0]; body = d[p+8:p+8+ln]
        if tag == b"PNTS":
            n = ln // 12
            verts = [struct.unpack(">fff", body[i*12:i*12+12]) for i in range(n)]
            break
        p += 8 + ln + (ln & 1)
    r = max(math.hypot(x, z) for (x, y, z) in verts)
    _rcache[name] = r
    return r


# ── camera path parse (CameraMotion block) ───────────────────────────────────
def parse_camera_keys(lines):
    n = len(lines); li = 0
    while li < n:
        if lines[li].strip().startswith("CameraMotion"):
            def nxt(m):
                while m < n and lines[m].strip() == "": m += 1
                return m
            k = nxt(li + 1); k = nxt(k + 1)          # channels, then nkeys
            nk = int(lines[k].split()[0]); k = nxt(k + 1)
            keys = []
            for _ in range(nk):
                vals = [float(x) for x in lines[k].split()]
                k2 = nxt(k + 1); fr = int(float(lines[k2].split()[0]))
                keys.append((fr, vals[0], vals[1], vals[2]))
                k = nxt(k2 + 1)
            return keys
        li += 1
    sys.exit("no CameraMotion")


# ── ship path parse (from chase_camera.py) ───────────────────────────────────
def parse_ship_keys(lines, want_base):
    n = len(lines); li = 0
    while li < n:
        s = lines[li].strip()
        if s.startswith("LoadObject") and basename(s[len("LoadObject"):]) == want_base:
            j = li + 1; mo = None
            while j < n:
                t = lines[j].strip()
                if t.startswith("LoadObject") or t.startswith("AddLight"):
                    break
                if t.startswith("ObjectMotion"):
                    mo = j; break
                j += 1
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


def sample_path(keys):
    pos = {}
    for f in range(FIRST_FRAME, LAST_FRAME + 1):
        if f <= keys[0][0]:
            pos[f] = keys[0][1:]
        elif f >= keys[-1][0]:
            pos[f] = keys[-1][1:]
        else:
            for i in range(len(keys) - 1):
                a, b = keys[i], keys[i+1]
                if a[0] <= f <= b[0]:
                    t = (f - a[0]) / (b[0] - a[0]) if b[0] != a[0] else 0.0
                    pos[f] = tuple(a[1+c] + t*(b[1+c]-a[1+c]) for c in range(3))
                    break
    return pos


def tangent(pos, f):
    a = pos[max(FIRST_FRAME, f-1)]; b = pos[min(LAST_FRAME, f+1)]
    v = (b[0]-a[0], b[2]-a[2]); L = math.hypot(*v) or 1.0
    return (v[0]/L, v[1]/L)   # (dx, dz) horizontal unit


def hash01(i):
    x = (i * 2654435761 + 0x9e3779b9) & 0xFFFFFFFF
    x ^= x >> 16; x = (x * 0x7feb352d) & 0xFFFFFFFF
    x ^= x >> 15; x = (x * 0x846ca68b) & 0xFFFFFFFF
    x ^= x >> 16
    return (x & 0xFFFFFF) / float(0x1000000)


# Wall palette: (lwo, scale_lo, scale_hi, orient_along_path)
WALLS  = [("m2.lwo", 1.0, 1.7, True), ("m3.lwo", 0.7, 1.1, False),
          ("m5.lwo", 1.8, 2.7, False), ("m1.lwo", 1.4, 2.3, False)]
SPIRES = [("m4.lwo", 1.6, 2.8, False), ("m5.lwo", 1.5, 2.4, False),
          ("m1.lwo", 1.3, 2.0, False)]


def build_block(lwo, x, y, z, h, scale):
    return [
        "LoadObject MY_OBJEC\\DEMO\\chasing\\" + lwo,
        "ShowObject 8 7",
        "ObjectMotion (unnamed)",
        "  9",
        "  1",
        "  %s %s %s %s 0 0 %s %s %s" % (fmt(x), fmt(y), fmt(z), fmt(h),
                                        fmt(scale), fmt(scale), fmt(scale)),
        "  0 0 0 0 0",
        "EndBehavior 1",
        "LockedChannels 8",
        "ShadowOptions 7",
        "",
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--f0", type=int, default=990)
    ap.add_argument("--f1", type=int, default=1360)
    ap.add_argument("--step", type=int, default=15)
    ap.add_argument("--base-clear", type=float, default=2150.0)
    ap.add_argument("--amp-clear", type=float, default=780.0)
    ap.add_argument("--wavelen", type=float, default=140.0)
    ap.add_argument("--min-face", type=float, default=1250.0)
    ap.add_argument("--min-face-cam", type=float, default=2800.0,
                    help="hard minimum near-face clearance from the CAMERA path "
                         "(the trailing cam swings wide on turns; a wall inside "
                         "this radius fills the frame / buries the shot)")
    ap.add_argument("--count", type=int, default=-1,
                    help="0 = remove the corridor block entirely (revert)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")

    # strip any existing corridor block (idempotent)
    b = next((i for i, l in enumerate(lines) if l.strip() == BEGIN), None)
    e = next((i for i, l in enumerate(lines) if l.strip() == END), None)
    if b is not None and e is not None and e >= b:
        del lines[b:e+1]

    if args.count == 0:
        out = "\n".join(lines)
        if not args.dry_run:
            open(LWS, "w", encoding="latin-1").write(out)
        print("corridor removed" + ("  (LWS byte-identical)" if out == raw else ""))
        return

    s1 = parse_ship_keys(lines, "ship1.lwo"); pos1 = sample_path(s1)
    s2 = parse_ship_keys(lines, "ship2.lwo"); pos2 = sample_path(s2)

    # dense ship-path point cloud for the safety re-check (both ships, padded)
    guard = []
    for f in range(args.f0 - 120, args.f1 + 120):
        guard.append(pos1[max(FIRST_FRAME, min(LAST_FRAME, f))])
        guard.append(pos2[max(FIRST_FRAME, min(LAST_FRAME, f))])

    # dense CAMERA-path point cloud — the trailing cam swings wide on turns and
    # is what buried the shot last time. Clear it by a larger margin.
    cam = parse_camera_keys(lines); posC = sample_path(cam)
    cam_guard = [posC[max(FIRST_FRAME, min(LAST_FRAME, f))]
                 for f in range(args.f0 - 120, args.f1 + 120)]

    def face_clear(cx, cz, rworld):
        dmin = min(math.hypot(cx - g[0], cz - g[2]) for g in guard)
        return dmin - rworld

    def face_clear_cam(cx, cz, rworld):
        dmin = min(math.hypot(cx - g[0], cz - g[2]) for g in cam_guard)
        return dmin - rworld

    def clearance(f, side):
        ph = (f - args.f0) / args.wavelen * 2*math.pi
        s = math.sin(ph) if side > 0 else math.sin(ph + math.pi)
        return args.base_clear + args.amp_clear * s

    insts = []          # (lwo, x, y, z, h, scale)
    idx = 0
    frames = list(range(args.f0, args.f1 + 1, args.step))
    for f in frames:
        p = pos1[f]
        dx, dz = tangent(pos1, f)
        nx, nz = dz, -dx                     # right-hand perpendicular (horizontal)
        headTan = math.degrees(math.atan2(dx, dz))   # LW heading of the path
        clearL = clearance(f, -1); clearR = clearance(f, +1)
        for side, clr in ((-1, clearL), (+1, clearR)):
            lwo, slo, shi, orient = WALLS[idx % len(WALLS)]
            scale = slo + (shi - slo) * hash01(idx * 7 + 3)
            rworld = lwo_radius_xz(lwo) * scale
            off = clr + rworld
            cx = p[0] + side * nx * off
            cz = p[2] + side * nz * off
            # safety: push outward until the near face clears both ship paths
            # AND the (wider) camera path
            for _ in range(48):
                if (face_clear(cx, cz, rworld) >= args.min_face and
                        face_clear_cam(cx, cz, rworld) >= args.min_face_cam):
                    break
                cx += side * nx * 200.0; cz += side * nz * 200.0
            h = headTan if orient else (hash01(idx*11+5) * 360.0 - 180.0)
            insts.append((lwo, cx, 0.0, cz, h, scale))
            idx += 1
        # a spire on the WIDER side every other sample (an obstacle to weave past)
        if (f // args.step) % 2 == 0:
            side = -1 if clearL > clearR else +1
            lwo, slo, shi, _ = SPIRES[idx % len(SPIRES)]
            scale = slo + (shi - slo) * hash01(idx * 13 + 1)
            rworld = lwo_radius_xz(lwo) * scale
            clr = 1500.0
            off = clr + rworld
            cx = p[0] + side * nx * off; cz = p[2] + side * nz * off
            for _ in range(48):
                if (face_clear(cx, cz, rworld) >= args.min_face and
                        face_clear_cam(cx, cz, rworld) >= args.min_face_cam):
                    break
                cx += side * nx * 200.0; cz += side * nz * 200.0
            h = hash01(idx*17+2) * 360.0 - 180.0
            insts.append((lwo, cx, 0.0, cz, h, scale))
            idx += 1

    block = [BEGIN]
    for lwo, x, y, z, h, sc in insts:
        block += build_block(lwo, x, y, z, h, sc)
    block += [END, ""]

    # insert before the first AmbientColor (end of the object section)
    ai = next((i for i, l in enumerate(lines) if l.strip().startswith("AmbientColor")), None)
    if ai is None:
        sys.exit("no AmbientColor anchor")
    new_lines = lines[:ai] + block + lines[ai:]
    out = "\n".join(new_lines)

    if args.dry_run:
        from collections import Counter
        c = Counter(i[0] for i in insts)
        print("would add %d instances: %s" % (len(insts), dict(c)))
        for lwo, x, y, z, h, sc in insts[:8]:
            print("  %-8s X=%8.0f Z=%8.0f H=%6.1f s=%.2f" % (lwo, x, z, h, sc))
        print("  ...")
        return

    open(LWS, "w", encoding="latin-1").write(out)
    print("added %d corridor instances (frames %d..%d)" % (len(insts), args.f0, args.f1))
    print("NOW RUN: tools/chase_sea_level.py  then  tools/chase_camera.py, then regen")


if __name__ == "__main__":
    main()
