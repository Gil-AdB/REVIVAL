#!/usr/bin/env python3
"""HOLE detector for the v4 undisplaced arm, control-referenced.

    tools/v4_tear_cover.py <controlDir> <latticeDir> [--label X]

The ground-truth tear detector of the campaign (tools/tear_detect.py + the
--refplane_dump reference planes) lives on rev-dispfix and is not on this
branch.  For an UNDISPLACED arm it is not the instrument that is needed
anyway: with the amplitude forced to 0 the lattice covers exactly the same
planes as the control (--v4_flat, the authored triangles through the same
pipeline), so a pixel the control rasterises and the lattice does not IS a
hole, with no reference renderer in the loop and no tolerance to argue about.

Reports, per pose:
  holes      control rasterised something, lattice rasterised NOTHING (z16 == 0)
  holes_st   ... and what the control had there was STONE
  new        lattice rasterised something the control did not
Exit 0 when holes == 0.
"""
import os, re, sys, collections
import numpy as np

STONE = {"rooms", "floor", "rooms::mirUV", "floor::mirUV"}

def find(d, suf):
    for f in sorted(os.listdir(d)):
        if f.endswith(suf):
            return os.path.join(d, f)
    return None

def matnames(p):
    names = {}
    if os.path.exists(p):
        for line in open(p, errors="replace"):
            m = re.match(r"\[GBUFDUMP\] id=(\d+) name=(.*)$", line.strip())
            if m: names[int(m.group(1))] = m.group(2)
    return names

def main():
    A, B = sys.argv[1], sys.argv[2]
    label = sys.argv[sys.argv.index("--label")+1] if "--label" in sys.argv else "pose"
    za, zb = find(A, "_depth.z16"), find(B, "_depth.z16")
    ma, mb = find(A, "_mat.u32"), find(B, "_mat.u32")
    if not (za and zb and ma and mb):
        print(f"{label}: MISSING planes"); sys.exit(2)
    Za = np.frombuffer(open(za,"rb").read(), dtype="<u2")
    Zb = np.frombuffer(open(zb,"rb").read(), dtype="<u2")
    n = min(Za.size, Zb.size); Za, Zb = Za[:n], Zb[:n]
    Ma = np.frombuffer(open(ma,"rb").read(), dtype="<u4")[:n]
    names = matnames(os.path.join(A, "log.txt"))
    sent = (Ma == 0xFFFFFFFF) | (Ma == 0xFFFFFFFE)
    ids  = np.where(sent, 255, (Ma >> 20) & 0xFF)
    stone_ids = np.array([i for i, nm in names.items() if nm in STONE] or [-1])
    is_stone = np.isin(ids, stone_ids) & ~sent

    hole = (Za != 0) & (Zb == 0)
    new  = (Za == 0) & (Zb != 0)
    holes, holes_st, news = int(hole.sum()), int((hole & is_stone).sum()), int(new.sum())
    where = np.nonzero(hole)[0][:8]
    w = 1920
    loc = [(int(i % w), int(i // w), names.get(int(ids[i]), f"id{int(ids[i])}")) for i in where]
    print(f"{label}: holes={holes} holes_on_stone={holes_st} new_coverage={news} px={n}"
          + (f" at={loc}" if holes else ""))
    sys.exit(0 if holes == 0 else 1)

main()
