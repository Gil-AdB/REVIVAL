#!/usr/bin/env python3
"""Weave the two chase ships' flight path into a serpentine through the gorge
(default frames ~1000-1350), so the canyon corridor visibly meanders.

Family: tools/chase_bank.py / chase_camera.py — in-place, parameterized.
Authored-first: modifies the Ship1 + ship2 ObjectMotion POSITION keys (X/Z only)
in the LWS; Y, rotation, scale untouched. A lateral serpentine offset,
perpendicular to the local path tangent and windowed (cosine taper at the ends
so it blends with the un-woven path outside the gorge), is ADDED to each key
whose frame is in [f0,f1].

Both ships get the same world-space offset(frame) so they stay in formation
(ship2 trails Ship1 down the same meander). Run this on the 4a-committed
(un-woven) LWS, THEN re-run tools/chase_corridor.py (walls re-hug the woven
path), tools/chase_sea_level.py, tools/chase_camera.py, then regen.

NOT idempotent (it adds to the current X/Z). To re-tune, `git checkout
Authoring/chase/CHASE.LWS` back to the un-woven state first, then re-run.
--amp 0 is a no-op (offset 0 everywhere → byte-identical).

Usage:
    tools/chase_weave.py [--amp U] [--wavelen F] [--f0 F --f1 F]
                         [--taper F] [--phase D] [--dry-run]
    --amp      peak lateral offset in world units (default 1600).
    --wavelen  serpentine wavelength in frames (default 175).
    --f0/--f1  woven frame window (default 1000..1350).
    --taper    cosine ease-in/out length at each window end (default 70).
    --phase    phase offset in degrees (default 0).
"""
import os, sys, math, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS  = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")
SHIPS = {"ship1.lwo", "ship2.lwo"}


def fmt(v):
    if abs(v) < 1e-9:
        return "0"
    s = "%.4f" % v
    return s.rstrip("0").rstrip(".")


def basename(p):
    return p.replace("/", "\\").split("\\")[-1].strip().lower()


def set_token(line, idx, s):
    lead = line[:len(line) - len(line.lstrip())]
    toks = line.split(); toks[idx] = s
    return lead + " ".join(toks)


def parse_motion(lines, mo):
    n = len(lines)
    def nxt(m):
        while m < n and lines[m].strip() == "": m += 1
        return m
    k = nxt(mo + 1); k = nxt(k + 1)
    nk = int(lines[k].split()[0]); k = nxt(k + 1)
    keys = []
    for _ in range(nk):
        vi = k; vals = [float(x) for x in lines[vi].split()]
        k = nxt(vi + 1); fi = k; fr = int(float(lines[fi].split()[0]))
        keys.append({"vi": vi, "fi": fi, "vals": vals, "frame": fr})
        k = nxt(fi + 1)
    return keys


def window(f, f0, f1, taper):
    if f < f0 or f > f1:
        return 0.0
    w = 1.0
    if f < f0 + taper:
        w = 0.5 - 0.5 * math.cos(math.pi * (f - f0) / taper)
    if f > f1 - taper:
        w = min(w, 0.5 - 0.5 * math.cos(math.pi * (f1 - f) / taper))
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--amp", type=float, default=1600.0)
    ap.add_argument("--wavelen", type=float, default=175.0)
    ap.add_argument("--f0", type=int, default=1000)
    ap.add_argument("--f1", type=int, default=1350)
    ap.add_argument("--taper", type=float, default=70.0)
    ap.add_argument("--phase", type=float, default=0.0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")

    # locate ship motions + build a per-ship frame->pos map for tangent calc
    ships = {}
    for li, line in enumerate(lines):
        s = line.strip()
        if not s.startswith("LoadObject"):
            continue
        base = basename(s[len("LoadObject"):])
        if base not in SHIPS:
            continue
        mo = next((j for j in range(li + 1, len(lines))
                   if lines[j].strip().startswith("ObjectMotion")), None)
        ships[base] = parse_motion(lines, mo)

    ph = math.radians(args.phase)

    def lateral(f):
        return args.amp * math.sin(2*math.pi*(f-args.f0)/args.wavelen + ph) \
               * window(f, args.f0, args.f1, args.taper)

    changed = 0
    for base, keys in ships.items():
        # tangent at each key from neighbouring keys (XZ)
        for i, k in enumerate(keys):
            f = k["frame"]
            w = lateral(f)
            if abs(w) < 1e-9:
                continue
            a = keys[max(0, i-1)]["vals"]; b = keys[min(len(keys)-1, i+1)]["vals"]
            dx = b[0]-a[0]; dz = b[2]-a[2]
            L = math.hypot(dx, dz) or 1.0
            dx /= L; dz /= L
            nx, nz = dz, -dx          # right-hand perpendicular
            newx = k["vals"][0] + nx * w
            newz = k["vals"][2] + nz * w
            lines[k["vi"]] = set_token(lines[k["vi"]], 0, fmt(newx))
            lines[k["vi"]] = set_token(lines[k["vi"]], 2, fmt(newz))
            changed += 1
        print(f"{base}: wove {sum(1 for k in keys if abs(lateral(k['frame']))>1e-9)} keys")

    out = "\n".join(lines)
    if args.dry_run:
        print(f"dry-run: would rewrite {changed} key positions "
              f"(amp={args.amp} wavelen={args.wavelen} window {args.f0}..{args.f1})")
        return
    open(LWS, "w", encoding="latin-1").write(out)
    print(f"wove {changed} key positions" +
          ("  (LWS byte-identical)" if out == raw else ""))
    print("NOW RUN: chase_corridor.py -> chase_sea_level.py -> chase_camera.py -> regen")


if __name__ == "__main__":
    main()
