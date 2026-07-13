#!/usr/bin/env python3
"""Re-author the chase camera as a trail-follow chase cam.

Family: tools/chase_bank.py / tools/chase_sea_level.py — in-place, idempotent,
parameterized, re-runnable. Authored-first: the result is CHASE.LWS
CameraMotion keyframes, no engine change, editor-visible after lwsread_legacy
regen.

The original chase camera is ONE 18-key look-at dolly (TargetObject 79 =
Ship1). It trails far behind and high, so both ships subtend a few pixels for
most of the timeline, and on the mm7 pass (frames ~1074-1201) a high dolly
pokes the ridge mesh the ships thread a low pass through.

The fix (this tool): place the camera on the SHIPS' OWN trail. The two ships
fly a demonstrably clear corridor through the canyon; a camera that rides that
same corridor is clear too. For each camera keyframe we put the camera `back`
arc-length behind ship2 (the trailing pursuer) ALONG ship2's actual traveled
path — not along the current tangent, which would cut corners into walls — then
lift it `lift` above that trail point and slide it `side` laterally so the two
ships separate in frame instead of overlapping. Look-at stays TargetObject 79
(Ship1), so only camera POSITIONS are authored; the H/P/B columns are written 0
(the look-at overrides them). The result: ship2 large in the foreground,
Ship1 the hero ahead — a real fast-chase read — with wall clearance inherited
from the ships.

Idempotence: every camera key is a pure function of the ship motion keys +
the (back, lift, side, zoom, step) params — never of the current camera. Re-run
reproduces exactly.

Usage:
    tools/chase_camera.py [--back N] [--lift N] [--side N]
                          [--zoom Z] [--step F] [--dry-run]
    --back  arc-length (LW units) the camera trails behind ship2. Default 1900.
    --lift  height (LW units) above the trail point. Default 150.
    --side  lateral offset (LW units); +right of travel. Default 300.
    --zoom  ZoomFactor to write (narrower = more presence). Default keep (3.2).
    --step  frames between authored camera keys. Default 34.
    --dry-run  print keys, write nothing.
"""
import os, sys, math, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")

FIRST_FRAME, LAST_FRAME = 0, 1700


def basename(path):
    return path.replace("/", "\\").split("\\")[-1].strip().lower()


def fmt(v):
    if abs(v) < 1e-9:
        return "0"
    s = "%.4f" % v
    return s.rstrip("0").rstrip(".")


def parse_ship_keys(lines, want_base):
    """Return list of (frame, x, y, z) for the named LoadObject's motion."""
    n = len(lines); li = 0
    while li < n:
        s = lines[li].strip()
        if s.startswith("LoadObject") and basename(s[len("LoadObject"):]) == want_base:
            j = li + 1; mo = None
            while j < n:
                t = lines[j].strip()
                if t.startswith("LoadObject") or t.startswith("AddLight") \
                        or t.startswith("ShowCamera") or t.startswith("AmbientColor"):
                    break
                if t.startswith("ObjectMotion"):
                    mo = j; break
                j += 1
            def nxt(m):
                while m < n and lines[m].strip() == "":
                    m += 1
                return m
            k = nxt(mo + 1)               # channels
            k = nxt(k + 1)                # nkeys
            nk = int(lines[k].split()[0])
            k = nxt(k + 1)
            keys = []
            for _ in range(nk):
                vals = [float(x) for x in lines[k].split()]
                k2 = nxt(k + 1)
                fr = int(float(lines[k2].split()[0]))
                keys.append((fr, vals[0], vals[1], vals[2]))
                k = nxt(k2 + 1)
            return keys
        li += 1
    sys.exit("no LoadObject %s" % want_base)


def sample_path(keys):
    """Per-frame linear position over [FIRST_FRAME, LAST_FRAME]."""
    pos = {}
    for f in range(FIRST_FRAME, LAST_FRAME + 1):
        if f <= keys[0][0]:
            pos[f] = keys[0][1:]
        elif f >= keys[-1][0]:
            pos[f] = keys[-1][1:]
        else:
            for i in range(len(keys) - 1):
                a, b = keys[i], keys[i + 1]
                if a[0] <= f <= b[0]:
                    t = (f - a[0]) / (b[0] - a[0]) if b[0] != a[0] else 0.0
                    pos[f] = tuple(a[1 + c] + t * (b[1 + c] - a[1 + c]) for c in range(3))
                    break
    return pos


def build_arc(pos):
    arc = {FIRST_FRAME: 0.0}
    for f in range(FIRST_FRAME + 1, LAST_FRAME + 1):
        pa, pb = pos[f - 1], pos[f]
        d = math.dist(pa, pb)
        arc[f] = arc[f - 1] + d
    return arc


def dir_at(pos, f):
    a = pos[max(FIRST_FRAME, f - 1)]
    b = pos[min(LAST_FRAME, f + 1)]
    v = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    L = math.sqrt(v[0]**2 + v[1]**2 + v[2]**2) or 1.0
    return (v[0] / L, v[1] / L, v[2] / L)


def point_at_arc(pos, arc, s):
    """(point, srcframe) at cumulative arc-length s. Extrapolate outside range."""
    if s <= 0.0:
        d = dir_at(pos, FIRST_FRAME)
        p0 = pos[FIRST_FRAME]
        return (p0[0] + d[0] * s, p0[1] + d[1] * s, p0[2] + d[2] * s), FIRST_FRAME
    total = arc[LAST_FRAME]
    if s >= total:
        d = dir_at(pos, LAST_FRAME)
        pL = pos[LAST_FRAME]
        e = s - total
        return (pL[0] + d[0] * e, pL[1] + d[1] * e, pL[2] + d[2] * e), LAST_FRAME
    lo, hi = FIRST_FRAME, LAST_FRAME
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if arc[mid] <= s:
            lo = mid
        else:
            hi = mid
    seg = arc[hi] - arc[lo]
    t = (s - arc[lo]) / seg if seg > 1e-9 else 0.0
    a, b = pos[lo], pos[hi]
    return tuple(a[c] + t * (b[c] - a[c]) for c in range(3)), lo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--back", type=float, default=1900.0)
    ap.add_argument("--lift", type=float, default=150.0)
    ap.add_argument("--side", type=float, default=300.0)
    ap.add_argument("--zoom", type=float, default=None)
    ap.add_argument("--step", type=int, default=34)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")

    s2 = parse_ship_keys(lines, "ship2.lwo")
    pos2 = sample_path(s2)
    arc2 = build_arc(pos2)

    # camera key frames: FIRST_FRAME..LAST_FRAME every `step`, always include LAST
    frames = list(range(FIRST_FRAME, LAST_FRAME + 1, args.step))
    if frames[-1] != LAST_FRAME:
        frames.append(LAST_FRAME)

    keys = []
    for cf in frames:
        s = arc2[cf] - args.back
        base, srcf = point_at_arc(pos2, arc2, s)
        # travel dir & right vector at the trail point
        d = dir_at(pos2, srcf)
        dh = (d[0], 0.0, d[2])
        Lh = math.sqrt(dh[0]**2 + dh[2]**2) or 1.0
        dh = (dh[0] / Lh, 0.0, dh[2] / Lh)
        right = (dh[2], 0.0, -dh[0])   # rotate travel +90 about Y
        cam = (base[0] + right[0] * args.side,
               base[1] + args.lift,
               base[2] + right[2] * args.side)
        keys.append((cf, cam))

    # emit block
    out = ["CameraMotion (unnamed)", "  9", "  %d" % len(keys)]
    for cf, cam in keys:
        out.append("  %s %s %s 0 0 0 1 1 1" % (fmt(cam[0]), fmt(cam[1]), fmt(cam[2])))
        out.append("  %d 0 0 0 0" % cf)

    if args.dry_run:
        print("\n".join(out))
        print("# %d keys  back=%.0f lift=%.0f side=%.0f step=%d"
              % (len(keys), args.back, args.lift, args.side, args.step))
        return

    # splice: replace lines from 'CameraMotion' through the line BEFORE EndBehavior
    n = len(lines); ci = None
    for i, ln in enumerate(lines):
        if ln.strip().startswith("CameraMotion"):
            ci = i; break
    if ci is None:
        sys.exit("no CameraMotion block")
    # find EndBehavior that closes the camera motion (first after ci)
    ei = None
    for i in range(ci + 1, n):
        if lines[i].strip().startswith("EndBehavior"):
            ei = i; break
    new_lines = lines[:ci] + out + lines[ei:]

    if args.zoom is not None:
        for i in range(ei, min(ei + 20, len(new_lines))):
            if new_lines[i].strip().startswith("ZoomFactor"):
                new_lines[i] = "ZoomFactor %f" % args.zoom
                break

    text = "\n".join(new_lines)
    with open(LWS, "w", encoding="latin-1") as f:
        f.write(text)
    print("wrote %d camera keys (back=%.0f lift=%.0f side=%.0f step=%d zoom=%s)"
          % (len(keys), args.back, args.lift, args.side, args.step,
             ("%.2f" % args.zoom) if args.zoom else "kept"))


if __name__ == "__main__":
    main()
