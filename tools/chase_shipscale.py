#!/usr/bin/env python3
"""Scale the two chase ships (Ship1 + ship2) larger in Authoring/chase/CHASE.LWS.

Family: tools/chase_bank.py / tools/chase_sea_level.py / tools/chase_camera.py —
in-place, idempotent, parameterized, re-runnable. Authored-first: the result is
LWS ObjectMotion per-key scale values, no engine change, editor-visible after
lwsread_legacy regen.

Each ObjectMotion key line is 9 floats: X Y Z  H P B  SX SY SZ  (the last three
are the per-key scale). Chase authored them all `1 1 1`. In the pulled-back
trail-follow camera the ships read small over the open-water stretches, so we
scale BOTH animated ships uniformly.

Only the Ship1.lwo (obj 79) and ship2.lwo (obj 80) ObjectMotion blocks are
touched — never the water plane, mountains, lights or camera. The 6 engine
glows are parented to the ships (ParentObject 79/80); the engine applies the
parent transform to child lights, so the glows scale WITH the ship and stay at
the nacelle tips (verified by render).

Idempotence: we SET the absolute scale (authored baseline = 1.0), a pure
function of the factor — never a running multiply — so re-running with the same
OR a different factor reproduces cleanly; factor 1.0 reverts to baseline
(byte-identical LWS).

Usage:  tools/chase_shipscale.py <scale>   [--ship1 S] [--ship2 S]
        <scale>    uniform scale applied to both ships (e.g. 1.6). 1.0 = revert.
        --ship1 S  override Ship1's scale (default = <scale>).
        --ship2 S  override ship2's scale (default = <scale>).
"""
import sys
import os
import argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")

SHIPS = {"ship1.lwo", "ship2.lwo"}


def fmt(v):
    if abs(v - 1.0) < 1e-9:
        return "1"
    if abs(v) < 1e-9:
        return "0"
    s = "%.4f" % v
    return s.rstrip("0").rstrip(".")


def basename(path):
    return path.replace("/", "\\").split("\\")[-1].strip().lower()


def set_scale(line, sx, sy, sz):
    """Replace tokens 6,7,8 (SX SY SZ) preserving lead whitespace + spacing."""
    lead = line[:len(line) - len(line.lstrip())]
    toks = line.split()
    toks[6], toks[7], toks[8] = fmt(sx), fmt(sy), fmt(sz)
    return lead + " ".join(toks)


def patch_ship(lines, mo_idx, scale):
    n = len(lines)

    def nxt(m):
        while m < n and lines[m].strip() == "":
            m += 1
        return m

    k = nxt(mo_idx + 1)          # channels line
    k = nxt(k + 1)               # nkeys line
    nk = int(lines[k].split()[0])
    k = nxt(k + 1)
    changed = 0
    for _ in range(nk):
        vi = k
        cur = lines[vi].split()
        newline = set_scale(lines[vi], scale, scale, scale)
        if newline != lines[vi]:
            lines[vi] = newline
            changed += 1
        k = nxt(vi + 1)          # frame line
        k = nxt(k + 1)           # next value line
    return changed, nk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scale", type=float)
    ap.add_argument("--ship1", type=float, default=None)
    ap.add_argument("--ship2", type=float, default=None)
    args = ap.parse_args()
    per = {"ship1.lwo": args.ship1 if args.ship1 is not None else args.scale,
           "ship2.lwo": args.ship2 if args.ship2 is not None else args.scale}

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")

    total = 0
    for li, line in enumerate(lines):
        s = line.strip()
        if not s.startswith("LoadObject"):
            continue
        base = basename(s[len("LoadObject"):])
        if base not in SHIPS:
            continue
        mo = next((j for j in range(li + 1, len(lines))
                   if lines[j].strip().startswith("ObjectMotion")), None)
        if mo is None:
            sys.exit(f"no ObjectMotion after {base}")
        n, nk = patch_ship(lines, mo, per[base])
        total += n
        print(f"{base}: scale={per[base]}  ({n}/{nk} keys changed)")

    out = "\n".join(lines)
    with open(LWS, "w", encoding="latin-1") as f:
        f.write(out)
    if out == raw:
        print("no change (LWS byte-identical)")
    print(f"total key lines rewritten = {total}")


if __name__ == "__main__":
    main()
