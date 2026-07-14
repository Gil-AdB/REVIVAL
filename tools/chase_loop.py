#!/usr/bin/env python3
"""Insert aerobatic 360 vertical LOOPS into a chase ship's flight path in
Authoring/chase/CHASE.LWS.

Family: tools/chase_bank.py / chase_camera.py — in-place, idempotent,
parameterized, re-runnable. Authored-first: the result is LWS ObjectMotion
keyframes, no engine change, editor-visible after lwsread_legacy regen.

A loop is a vertical circle superimposed on the ship's forward motion: over the
window [f0,f1] the ship rises, goes over the top (inverted), and comes back down
to rejoin its path, while its PITCH sweeps a full 360 (nose up -> inverted ->
nose down -> level). The base path inside the window is taken as a straight
interp between the keys just OUTSIDE the window, so re-running strips the prior
loop and regenerates identically (idempotent). Heading/bank are left to
chase_bank.py; only Position (0,1,2) and Pitch (channel idx 4) are authored here.

Usage:
    tools/chase_loop.py <ship1|ship2> <f0> <f1> [--radius R] [--samples N]
                        [--pitch-sign +1|-1] [--remove]
    f0,f1   loop frame window (pick a stretch of OPEN airspace, e.g. 560 620).
    --radius   loop radius in world units (default 3000; ~ship-scale drama).
    --samples  keys generated across the loop (default 16; smoothness).
    --pitch-sign  flip if the ship pitches the wrong way (default -1 = nose-up
                  climbing, matching the finale climb convention).
    --remove   strip the loop in [f0,f1] and restore the straight base (revert).
"""
import sys, os, math, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS  = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")


def fmt(v):
    if abs(v) < 1e-9:
        return "0"
    return ("%.4f" % v).rstrip("0").rstrip(".")


def basename(p):
    return p.replace("/", "\\").split("\\")[-1].strip().lower()


def find_motion(lines, want_base):
    """Return (mo_idx, chan_idx, nkeys_idx, first_key_idx) for the ship."""
    n = len(lines); li = 0
    while li < n:
        s = lines[li].strip()
        if s.startswith("LoadObject") and basename(s[len("LoadObject"):]) == want_base:
            j = li + 1
            while j < n and not lines[j].strip().startswith(("ObjectMotion", "LoadObject")):
                j += 1
            if j >= n or not lines[j].strip().startswith("ObjectMotion"):
                sys.exit("no ObjectMotion for %s" % want_base)
            def nxt(m):
                while m < n and lines[m].strip() == "": m += 1
                return m
            ci = nxt(j + 1); ki = nxt(ci + 1); fk = nxt(ki + 1)
            return j, ci, ki, fk
        li += 1
    sys.exit("no LoadObject %s" % want_base)


def parse_keys(lines, ki, fk):
    """Return list of (frame, [9 floats], vi, fi)."""
    n = len(lines)
    nk = int(lines[ki].split()[0])
    def nxt(m):
        while m < n and lines[m].strip() == "": m += 1
        return m
    keys = []; k = fk
    for _ in range(nk):
        k = nxt(k)
        vals = [float(x) for x in lines[k].split()]
        f2 = nxt(k + 1); fr = int(round(float(lines[f2].split()[0])))
        keys.append((fr, vals))
        k = nxt(f2 + 1)
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ship", choices=["ship1", "ship2"])
    ap.add_argument("f0", type=int)
    ap.add_argument("f1", type=int)
    ap.add_argument("--radius", type=float, default=3000.0)
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--pitch-sign", type=float, default=-1.0)
    ap.add_argument("--remove", action="store_true")
    args = ap.parse_args()
    base = args.ship + ".lwo"

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")
    mo, ci, ki, fk = find_motion(lines, base)
    keys = parse_keys(lines, ki, fk)

    # boundary keys just outside the window define the straight base path
    before = [k for k in keys if k[0] <= args.f0]
    after  = [k for k in keys if k[0] >= args.f1]
    if not before or not after:
        sys.exit("window %d..%d not inside %s's key range" % (args.f0, args.f1, base))
    kb, ka = before[-1], after[0]

    def base_pos(f):
        t = (f - kb[0]) / (ka[0] - kb[0]) if ka[0] != kb[0] else 0.0
        return [kb[1][c] + t * (ka[1][c] - kb[1][c]) for c in range(3)]

    # forward (horizontal) unit at window start, from the surrounding base
    d = [ka[1][c] - kb[1][c] for c in range(3)]
    fh = math.hypot(d[0], d[2]) or 1.0
    fx, fz = d[0] / fh, d[2] / fh
    tmpl = kb[1][:]   # scale/etc template from the boundary key

    # keys strictly inside the window are dropped (prior loop or straight run)
    kept = [k for k in keys if not (args.f0 < k[0] < args.f1)]

    new = []
    if not args.remove:
        for i in range(1, args.samples):
            f = args.f0 + (args.f1 - args.f0) * i / args.samples
            th = 2.0 * math.pi * i / args.samples
            b = base_pos(f)
            off_f = args.radius * math.sin(th)          # along forward
            off_u = args.radius * (1.0 - math.cos(th))  # vertical (up = +Y)
            x = b[0] + off_f * fx
            y = b[1] + off_u
            z = b[2] + off_f * fz
            v = tmpl[:]
            v[0], v[1], v[2] = x, y, z
            v[4] = args.pitch_sign * math.degrees(th)   # Pitch sweeps 0..360
            new.append((int(round(f)), v))

    allkeys = sorted(kept + new, key=lambda k: k[0])

    # rebuild the key block
    block = []
    for fr, v in allkeys:
        block.append("  " + " ".join(fmt(x) for x in v))
        block.append("  %d 0 0 0 0" % fr)

    # splice: replace from first-key line through the last key's frame line
    n = len(lines)
    def nxt(m):
        while m < n and lines[m].strip() == "": m += 1
        return m
    start = fk
    k = fk
    for _ in range(len(keys)):
        k = nxt(k); k = nxt(k + 1); k = nxt(k + 1)
    end = k   # one past the last frame line
    lines[ki] = "  %d" % len(allkeys)
    out_lines = lines[:start] + block + lines[end:]
    out = "\n".join(out_lines)
    open(LWS, "w", encoding="latin-1").write(out)
    print("%s: %s loop %d..%d (r=%.0f) -> %d keys (was %d)"
          % (base, "REMOVED" if args.remove else "added", args.f0, args.f1,
             args.radius, len(allkeys), len(keys)))
    print("NOW: regen (lwsread_legacy) + install")


if __name__ == "__main__":
    main()
