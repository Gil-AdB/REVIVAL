#!/usr/bin/env python3
"""Cross-section of the jamb band from [STONE-FINALV] lines.

The jamb wall plane is x=17.898 (normal +x), its freed border the vertical
line z=-58.014 (runs in y). For each vert on that plane: along=y,
across=z+58.014 (0 at the border, + into the wall), out=dsp*dir.x
(offset along the wall normal; + = proud of the authored plane).
Rows = 0.25u bins in y. Prints per-row profile across the band, plus the
same for the PARTNER sheet (plane z=-58.014, normal +z, border x=17.898).
"""
import re, sys, collections

pat = re.compile(
    r"\[STONE-FINALV\] '([^']+)' pos\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"dir\(([-+\d.]+),([-+\d.]+),([-+\d.]+)\) dsp ([-+\d.]+) "
    r"uv\(([-\d.]+),([-\d.]+)\) hRaw ([-\d.]+) hEff ([-\d.]+) "
    r"mean ([-\d.]+) cls (.)( WELD)?")

def load(path):
    rows = []
    for ln in open(path, errors="replace"):
        m = pat.search(ln)
        if not m:
            continue
        mat, x, y, z, dx, dy, dz, dsp, u, v, hraw, heff, mean, cls, weld = m.groups()
        rows.append(dict(mat=mat, x=float(x), y=float(y), z=float(z),
                         dx=float(dx), dy=float(dy), dz=float(dz),
                         dsp=float(dsp), hraw=float(hraw), heff=float(heff),
                         mean=float(mean), cls=cls, weld=bool(weld)))
    return rows

def sheet(rows, plane_axis, plane_val, normal_axis, border_axis, border_val, border_sign):
    """verts on |plane_axis-plane_val|<0.02; across=(border_axis-border_val)*sign"""
    out = []
    for r in rows:
        if abs(r[plane_axis] - plane_val) > 0.02:
            continue
        across = (r[border_axis] - border_val) * border_sign
        if across < -0.05:
            continue
        outofplane = r["dsp"] * r["d" + normal_axis]
        out.append((r["y"], across, outofplane, r["cls"], r["weld"], r["dsp"],
                    r["hraw"], r["heff"]))
    return out

def profile(name, data):
    print(f"== {name}: {len(data)} verts ==")
    rows = collections.defaultdict(list)
    for y, across, out, cls, weld, dsp, hraw, heff in data:
        rows[round(y * 4) / 4].append((across, out, cls, weld, hraw, heff))
    xb = [0.0, 0.02, 0.06, 0.10, 0.16, 0.25, 0.40, 0.65, 1.0, 1.6, 99.0]
    hdr = "  y-row |" + "".join(f" {a:>5.2f}-" for a in xb[:-1])
    print(hdr)
    for yk in sorted(rows):
        cells = []
        for i in range(len(xb) - 1):
            sel = [o for a, o, c, w, hr, he in rows[yk] if xb[i] <= a < xb[i + 1]]
            cells.append(f"{1000*sum(sel)/len(sel):+6.0f}" if sel else "     .")
        print(f"  {yk:5.2f} |" + " ".join(cells))
    print("  (cells: mean out-of-plane offset, MILLI-units; + = proud of plane)")

rows = load(sys.argv[1])
print(f"total FINALV verts: {len(rows)}")
# jamb sheet: wall plane x=17.898, normal x, border z=-58.014, wall extends z>-58.014?
# 16y: border z=-58.014, wall on visible side z>-58.014 toward -52 => across=z+58.014
profile("JAMB SHEET x=17.898 (normal x), border z=-58.014",
        sheet(rows, "x", 17.898, "x", "z", -58.014, +1.0))
# partner sheet through the same corner: plane z=-58.014? partner of the arris
profile("PARTNER SHEET z=-58.014 (normal z), border x=17.898",
        sheet(rows, "z", -58.014, "z", "x", 17.898, +1.0))
# class census
cc = collections.Counter((r["cls"], r["weld"]) for r in rows)
print("class census:", dict(cc))

# one-line matrix metric: proudness + undulation + sheet asymmetry
def metric(rows):
    j = sheet(rows, "x", 17.898, "x", "z", -58.014, +1.0)
    p = sheet(rows, "z", -58.014, "z", "x", 17.898, +1.0)
    jb = [(y, o) for y, a, o, c, w, d, hr, he in j if a < 0.02]
    pb = [(y, o) for y, a, o, c, w, d, hr, he in p if a < 0.02]
    proud = max([o for _, o in jb + pb], default=0.0)
    undul = (max([o for _, o in jb], default=0) - min([o for _, o in jb], default=0))
    import statistics
    asym = 0.0
    if jb and pb:
        # welded corner verts appear in both sheets; asymmetry = mean |out_j| / mean |out_p|
        mj = statistics.mean(abs(o) for _, o in jb)
        mp = statistics.mean(abs(o) for _, o in pb)
        asym = mj / mp if mp > 1e-9 else 0.0
    return proud, undul, asym, len(jb), len(pb)

if len(sys.argv) > 2 and sys.argv[2] == "brief":
    pr, un, asy, nj, np_ = metric(rows)
    print(f"METRIC proud={1000*pr:+.1f}m undul={1000*un:.1f}m asym={asy:.2f} nj={nj} np={np_}")

# corner-vs-block mismatch: weld verts on stone (non-groove) samples should sit
# at their OWN block top: |cosHalf*dsp - amp*(hRaw-mean)| -> 0 under block_level
if len(sys.argv) > 2 and sys.argv[2] in ("brief", "mm"):
    AMP, CH = 0.300, 0.851
    mm = [abs(CH * r["dsp"] - AMP * (r["hraw"] - r["mean"]))
          for r in rows if r["weld"] and (r["hraw"] - r["mean"]) > -0.08]
    if mm:
        import statistics
        print(f"MISMATCH n={len(mm)} mean={1000*statistics.mean(mm):.1f}m "
              f"max={1000*max(mm):.1f}m")
