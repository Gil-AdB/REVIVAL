#!/usr/bin/env python3
"""Level every chase mountain instance onto the water plane (LightWave Y=0).

Family: tools/chase_bank.py / tools/add_city_beam_flags.py — in-place,
idempotent, parameterized, re-runnable. Authored-first: the result is
CHASE.LWS motion-key Y values, no engine change, editor-visible after regen.

The problem (hand-placed in 1998, never leveled): the water plane sits at
LightWave Y=0 but every mountain instance floats a different small amount
above it (m1 +11, m2 +36, m3 +29.5, m4 +42, mm7 +111.5, m5 +21.5, big_m
+1535 — those are just the motion-key Y values). The camera dolly then
threads a canyon whose feet don't touch the sea, so mountain/water contacts
either hover with a hairline gap or shimmer.

The fix: for each mountain instance, drop it so its LOWEST world vertex sits
a few units BELOW the water (a small submerge, so the intersection is a clean
waterline — a base exactly at 0 z-fights/shimmers). We only rewrite the
motion-key Y; X/Z, all rotation, scale and pivot are preserved verbatim.

Idempotence (the important bit): the new Y is a pure function of IMMUTABLE
data — the LWO mesh, the instance rotation/scale/pivot — never of the current
Y. So re-running (same or different submerge) reproduces exactly; it never
drifts:

    new_posY = -submerge - scaleY * min_over_verts( [R(H,P,B)*(v - pivot)]_Y )

R is the LightWave rotation from the key's Heading/Pitch/Bank. Heading is
about the vertical (Y) axis and never touches Y, so it drops out; pitch/bank
are near-0 for the mountains but applied for correctness (a tilted mesh has
its true low corner found). Everything is LightWave-native space (the LWS/LWO
text coordinates), where the water is at Y=0 — no engine SwapYZ involved.

big_m is handled like the rest by default (--big-m-mode snap); it is a real
mountain whose feet the leveling drops to the sea. Use --big-m-mode skip to
leave it untouched if a render shows its waterline is intentional.

Usage:
    tools/chase_sea_level.py [--submerge N] [--dry-run]
                             [--big-m-mode {snap,skip}]
    --submerge  depth (LightWave units) each mountain's lowest vertex sits
                below Y=0. Default 8. 0 = base exactly at the waterline
                (may shimmer — not recommended).
    --dry-run   print the analysis (local base, current & new world base,
                delta) and write nothing.
"""
import sys
import os
import math
import struct
import argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWS = os.path.join(REPO, "Authoring", "chase", "CHASE.LWS")
LWO_DIR = os.path.join(REPO, "Authoring", "chase")

# LWO basenames that are mountains (everything the leveling touches). water /
# the two ships are excluded.
MOUNTAINS = {"m1.lwo", "m2.lwo", "m3.lwo", "m4.lwo", "m5.lwo",
             "mm7.lwo", "big_m.lwo"}


def fmt(v):
    """LightWave-style float: trim trailing zeros, plain 0 for zero."""
    if abs(v) < 1e-9:
        return "0"
    s = "%.4f" % v
    s = s.rstrip("0").rstrip(".")
    return s


def basename(path):
    return path.replace("/", "\\").split("\\")[-1].strip().lower()


def set_token(line, idx, value_str):
    """Replace token `idx`, preserving leading whitespace + single spaces."""
    lead = line[:len(line) - len(line.lstrip())]
    toks = line.split()
    toks[idx] = value_str
    return lead + " ".join(toks)


# --- LWO PNTS reader (LWOB IFF): PNTS chunk = N*3 big-endian float32 in
# native LightWave coordinates (X, Y-up, Z). Cached per file. ---
_pnts_cache = {}


def load_pnts(lwo_path):
    key = os.path.abspath(lwo_path)
    if key in _pnts_cache:
        return _pnts_cache[key]
    d = open(lwo_path, "rb").read()
    if d[0:4] != b"FORM" or d[8:12] not in (b"LWOB", b"LWO2"):
        sys.exit("%s: not an LWOB/LWO2 IFF file" % lwo_path)
    verts = None
    p = 12
    while p + 8 <= len(d):
        tag = d[p:p + 4]
        ln = struct.unpack(">I", d[p + 4:p + 8])[0]
        body = d[p + 8:p + 8 + ln]
        if tag == b"PNTS":
            n = ln // 12
            verts = [struct.unpack(">fff", body[i * 12:i * 12 + 12])
                     for i in range(n)]
            break
        p += 8 + ln + (ln & 1)
    if verts is None:
        sys.exit("%s: no PNTS chunk" % lwo_path)
    _pnts_cache[key] = verts
    return verts


def rot_y_component(u, h_deg, p_deg, b_deg):
    """Y component of R(H,P,B)*u in LightWave convention (bank about Z, then
    pitch about X, then heading about Y). Heading (about Y) leaves Y invariant,
    so it is omitted. u = (x, y, z)."""
    x, y, z = u
    b = math.radians(b_deg)
    pp = math.radians(p_deg)
    # bank about Z: y1 = x*sin b + y*cos b
    y1 = x * math.sin(b) + y * math.cos(b)
    z1 = z
    # pitch about X: y2 = y1*cos p - z1*sin p
    y2 = y1 * math.cos(pp) - z1 * math.sin(pp)
    return y2


def instance_world_min_y(lwo_path, rot, scale_y, pivot):
    """Minimum world Y over all mesh vertices for a placement at posY=0 (the
    pure-mesh contribution). new_posY = -submerge - this."""
    h, pch, bnk = rot
    px, py, pz = pivot
    verts = load_pnts(lwo_path)
    mn = None
    for (x, y, z) in verts:
        u = (x - px, y - py, z - pz)
        wy = scale_y * rot_y_component(u, h, pch, bnk)
        if mn is None or wy < mn:
            mn = wy
    return mn


def parse_instances(lines):
    """Walk LoadObject blocks; return a list of dicts for mountain instances:
    {base, lwo_path, vi (position value-line index), curY, rot, scaleY,
     pivot}."""
    out = []
    n = len(lines)
    li = 0
    while li < n:
        s = lines[li].strip()
        if s.startswith("LoadObject"):
            base = basename(s[len("LoadObject"):])
            # scan this block until the next LoadObject/AddLight/section
            j = li + 1
            mo = None
            pivot = (0.0, 0.0, 0.0)
            while j < n:
                t = lines[j].strip()
                if t.startswith("LoadObject") or t.startswith("AddLight") \
                        or t.startswith("ShowCamera") or t.startswith("AmbientColor"):
                    break
                if t.startswith("ObjectMotion") and mo is None:
                    mo = j
                if t.startswith("PivotPoint"):
                    pv = t.split()[1:]
                    pivot = (float(pv[0]), float(pv[1]), float(pv[2]))
                j += 1
            if base in MOUNTAINS and mo is not None:
                # first non-empty line after channels+nkeys is the value line
                k = mo + 1
                def nxt(m):
                    while m < n and lines[m].strip() == "":
                        m += 1
                    return m
                k = nxt(k)          # channels
                k = nxt(k + 1)      # nkeys
                vi = nxt(k + 1)     # first key's value line
                toks = [float(x) for x in lines[vi].split()]
                out.append({
                    "base": base,
                    "lwo_path": os.path.join(LWO_DIR, base),
                    "vi": vi,
                    "curY": toks[1],
                    "rot": (toks[3], toks[4], toks[5]),
                    "scaleY": toks[7],
                    "pivot": pivot,
                })
            li = j
            continue
        li += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--submerge", type=float, default=8.0)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--big-m-mode", choices=["snap", "skip"], default="snap")
    args = ap.parse_args()

    raw = open(LWS, encoding="latin-1").read()
    lines = raw.split("\n")
    insts = parse_instances(lines)

    changed = 0
    # group print by lwo
    by_lwo = {}
    for it in insts:
        by_lwo.setdefault(it["base"], []).append(it)

    print("submerge=%.3f  big_m=%s  instances=%d across %d LWOs"
          % (args.submerge, args.big_m_mode, len(insts), len(by_lwo)))
    print("%-10s %3s  %10s %10s %10s %10s" %
          ("lwo", "#", "curY", "curBaseY", "newY", "newBaseY"))
    for base in sorted(by_lwo):
        group = by_lwo[base]
        for idx, it in enumerate(group):
            meshmin = instance_world_min_y(it["lwo_path"], it["rot"],
                                           it["scaleY"], it["pivot"])
            cur_base = it["curY"] + meshmin
            skip = (base == "big_m.lwo" and args.big_m_mode == "skip")
            if skip:
                new_y = it["curY"]
                new_base = cur_base
            else:
                new_y = -args.submerge - meshmin
                new_base = -args.submerge
            newYs = fmt(new_y)
            cur_tok = lines[it["vi"]].split()[1]
            print("%-10s %3d  %10.3f %10.3f %10s %10.3f%s" %
                  (base, idx, it["curY"], cur_base, newYs, new_base,
                   "  [SKIP]" if skip else ""))
            if not skip and not args.dry_run and cur_tok != newYs:
                lines[it["vi"]] = set_token(lines[it["vi"]], 1, newYs)
                changed += 1

    if args.dry_run:
        print("dry-run: nothing written")
        return

    out = "\n".join(lines)
    with open(LWS, "w", encoding="latin-1") as f:
        f.write(out)
    print("wrote %d instance Y values%s" %
          (changed, "  (LWS byte-identical)" if out == raw else ""))


if __name__ == "__main__":
    main()
