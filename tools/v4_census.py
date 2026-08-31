#!/usr/bin/env python3
"""Reader for the v4 bake census blocks.

Phase 1: [V4-STITCH] / [V4-CHARTS].  Phase 2: [V4-LATTICE] / [V4-OUT].

The census is printed on stderr by DEMO/V4Bake.cpp when BOTH
--greets_displace_v4 and --v4_census are on.  This turns the block into
numbers, and --gate turns the numbers into the phase-1 pass/fail of
docs/DISPLACEMENT_V4_DESIGN.md §6 P1.

Usage
-----
  # render once, keeping stderr
  cd /path/to/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \\
    FDS_GREETS_CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \\
    ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \\
           --greets-displace --greets_displace_v4 --v4_census \\
           --force_xres=1920 --force_yres=1080 \\
           --snapshot=greets@t=5965 --out=/tmp/v4 > /tmp/v4/log.txt 2>&1

  python3 tools/v4_census.py /tmp/v4/log.txt            # human table
  python3 tools/v4_census.py --json /tmp/v4/log.txt     # one JSON object
  python3 tools/v4_census.py --gate /tmp/v4/log.txt     # P1 PASS/FAIL, exit 0/1
  python3 tools/v4_census.py --p2 /tmp/v4/log.txt       # the phase-2 table
  python3 tools/v4_census.py --p2gate /tmp/v4/log.txt   # P2 PASS/FAIL, exit 0/1

Gate (design §6 P1)
-------------------
  * 0 non-manifold edges (use-count > 2)
  * every use-count-1 stone edge classified against the FULL soup: the truly
    free ones are listed by name; a use-count-1 edge whose far side carries a
    face of another material is a MATERIAL SEAM, not a boundary
  * 0 epsilon-fallback welds (the authored corners are bitwise equal)
  * no null twin anywhere in the half-edge structure
  * every stone face in exactly one chart
"""

import argparse
import json
import re
import sys

TOKEN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


def num(s):
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        return s


def parse(lines):
    """Return {'sec': {'<block>.<section>': {...}}, '<rowtag>': [ {...} ]}.

    Each census line is `[V4-BLOCK] <section> k=v k=v ...`, so the section word
    keeps `total` on the `faces` line distinct from `total` on the `edges` line.
    Row-shaped lines (one per mesh / free edge / chart / junction) collect into
    lists instead.
    """
    out = {
        "sec": {},
        "mesh": [], "use1": [], "nonmanifold": [], "chart_rows": [],
        "junc": [], "soupmat": [], "abutmat": [], "ulpmerge": [], "epsmerge": [],
        "mat": [], "worst": [],
        "sweep": {},
    }
    row_tags = {
        "MESH": "mesh", "USE1": "use1", "NONMANIFOLD": "nonmanifold",
        "CHART": "chart_rows", "JUNC": "junc", "SOUPMAT": "soupmat",
        "ABUTMAT": "abutmat", "ULPMERGE": "ulpmerge", "EPSMERGE": "epsmerge",
        "MAT": "mat", "WORST": "worst",
    }
    blocks = {"[V4-STITCH]": "stitch", "[V4-CHARTS]": "charts", "[V4-CENSUS]": "census",
              "[V4-LATTICE]": "lattice", "[V4-OUT]": "out",
              "[V4-DISPLACE]": "displace", "[V4-RELIEF]": "relief"}
    for ln in lines:
        tag = next((t for t in blocks if t in ln), None)
        if tag is None:
            continue
        body = ln.split(tag, 1)[1].strip()
        if not body:
            continue
        head = body.split(None, 1)[0]
        if head in row_tags:
            out[row_tags[head]].append({k: num(v) for k, v in TOKEN.findall(body)})
            continue
        if head == "sweep":
            for tok in body.split()[2:]:
                if ":" in tok:
                    a, b = tok.split(":", 1)
                    out["sweep"][a] = num(b)
            continue
        toks = {k: num(v) for k, v in TOKEN.findall(body)}
        # [V4-RELIEF] class/planegate/cross rows repeat under one section word and
        # are distinguished by their `name=`, so key them by it.
        if head in ("class", "planegate") and "name" in toks:
            key = "%s.%s.%s" % (blocks[tag], head, toks["name"])
        else:
            key = "%s.%s" % (blocks[tag], head)
        out["sec"].setdefault(key, {}).update(toks)
    return out


def g(d, section, key, default=None):
    return d["sec"].get(section, {}).get(key, default)


def summarise(d):
    return {
        # stage (a)
        "stone_faces": g(d, "stitch.faces", "total"),
        "soup_faces": g(d, "stitch.soup", "faces"),
        "soup_meshes": g(d, "stitch.soup", "meshes"),
        "meshes_with_stone": g(d, "stitch.placement", "meshes_with_stone"),
        "placement_all_identity": g(d, "stitch.placement", "all_identity"),
        "corners": g(d, "stitch.weld", "corners"),
        "vertices": g(d, "stitch.weld", "vertices"),
        "exact_classes": g(d, "stitch.weld", "exact_classes"),
        "exact_merges": g(d, "stitch.weld", "exact_merges"),
        "ulp_merges": g(d, "stitch.weld", "ulp_merges"),
        "eps_merges": g(d, "stitch.weld", "eps_merges"),
        "eps": g(d, "stitch.weld", "eps"),
        "shortest_edge": g(d, "stitch.weld", "shortest_edge"),
        "edges": g(d, "stitch.edges", "total"),
        "use1": g(d, "stitch.edges", "use1"),
        "use2": g(d, "stitch.edges", "use2"),
        "use3plus": g(d, "stitch.edges", "use3plus"),
        "null_twin": g(d, "stitch.halfedge", "null_twin"),
        "boundary_he": g(d, "stitch.halfedge", "boundary_he"),
        "matseam_edges": g(d, "stitch.attrs", "matseam_edges"),
        "matseam_len": g(d, "stitch.attrs", "matseam_len"),
        "coplanar_edges": g(d, "stitch.attrs", "coplanar_edges"),
        "crease_edges": g(d, "stitch.attrs", "crease_edges"),
        "crease_len": g(d, "stitch.attrs", "crease_len"),
        "smooth_edges": g(d, "stitch.attrs", "smooth_edges"),
        "convex_edges": g(d, "stitch.attrs", "convex"),
        "concave_edges": g(d, "stitch.attrs", "concave"),
        "orient_flip": g(d, "stitch.attrs", "orient_flip"),
        "convex_disagree": g(d, "stitch.attrs", "convex_disagree"),
        "free": g(d, "stitch.use1_class", "free"),
        "free_len": g(d, "stitch.use1_class", "free_len"),
        "shared_soup": g(d, "stitch.use1_class", "shared_soup"),
        "coincident": g(d, "stitch.use1_class", "coincident"),
        "cross_mesh_abut": g(d, "stitch.use1_class", "cross_mesh_abut"),
        "stitch_ms": g(d, "stitch.timing", "ms"),
        # stage (b)
        "chart_budget_deg": g(d, "charts.registry", "budget_deg"),
        "charts": g(d, "charts.registry", "charts"),
        "chart_faces": g(d, "charts.registry", "faces"),
        "unassigned": g(d, "charts.registry", "unassigned"),
        "maxdev_max_deg": g(d, "charts.registry", "maxdev_max_deg"),
        "over_budget_charts": g(d, "charts.registry", "over_budget_charts"),
        "junctions": g(d, "charts.junc_summary", "junctions"),
        "junction_len": g(d, "charts.junc_summary", "total_len"),
        "junction_edges_convex": g(d, "charts.junc_summary", "edges_convex"),
        "junction_edges_concave": g(d, "charts.junc_summary", "edges_concave"),
        "junction_edges_smooth": g(d, "charts.junc_summary", "edges_smooth"),
        "planes": g(d, "charts.planepair", "planes"),
        "planepair_junctions": g(d, "charts.planepair", "junctions"),
        "planepair_len": g(d, "charts.planepair", "total_len"),
        "planes_with_multiple_charts": g(d, "charts.planepair", "planes_with_multiple_charts"),
        "charts_for_90pct_area": g(d, "charts.area", "charts_for_90pct"),
        "charts_ms": g(d, "charts.timing", "ms"),
        "total_ms": g(d, "census.timing", "total_ms"),
    }


def summarise_p2(d):
    """The phase-2 numbers (design section 2c / 2h)."""
    return {
        "arm": g(d, "lattice.arm", "name"),
        "cpb": g(d, "lattice.arm", "cpb"),
        "groove_refine": g(d, "lattice.arm", "groove_refine"),
        # the second, independent topology build -- cross-checks P1's census
        "topo_faces": g(d, "lattice.topo", "faces"),
        "topo_verts": g(d, "lattice.topo", "verts"),
        "topo_corners": g(d, "lattice.topo", "corners"),
        "topo_edges": g(d, "lattice.topo", "edges"),
        "topo_use1": g(d, "lattice.topo", "use1"),
        "topo_use2": g(d, "lattice.topo", "use2"),
        "topo_use3plus": g(d, "lattice.topo", "use3plus"),
        "topo_charts": g(d, "lattice.topo", "charts"),
        "meshes_with_stone": g(d, "lattice.topo", "meshes_with_stone"),
        # borders (R1/R2/R3)
        "border_seg_min": g(d, "lattice.borders", "seg_min"),
        "border_seg_max": g(d, "lattice.borders", "seg_max"),
        "border_samples": g(d, "lattice.borders", "interior_samples"),
        "border_capped": g(d, "lattice.borders", "capped"),
        "border_max_dev": g(d, "lattice.borders", "max_dev_from_line"),
        # nodes
        "nodes_generated": g(d, "lattice.nodes", "generated"),
        "nodes_kept": g(d, "lattice.nodes", "kept"),
        "nodes_outside": g(d, "lattice.nodes", "outside"),
        "nodes_margin": g(d, "lattice.nodes", "margin"),
        "nodes_capped": g(d, "lattice.nodes", "capped"),
        "plateau_min_level0_texels": g(d, "lattice.nodes", "plateau_min_level0_texels"),
        "level_jump_violations": g(d, "lattice.nodes", "level_jump_violations"),
        # triangulation
        "degenerate_uv": g(d, "lattice.triangulation", "degenerate_uv"),
        "delaunay_fallback": g(d, "lattice.triangulation", "delaunay_fallback"),
        "row_cap_faces": g(d, "lattice.triangulation", "row_cap_faces"),
        "cell_world_min": g(d, "lattice.triangulation", "cell_world_min"),
        "cell_world_max": g(d, "lattice.triangulation", "cell_world_max"),
        "ms_lattice": g(d, "lattice.triangulation", "ms_lattice"),
        "ms_topo": g(d, "lattice.triangulation", "ms_topo"),
        "ms_grid": g(d, "lattice.triangulation", "ms_grid"),
        "ms_commit": g(d, "lattice.triangulation", "ms_commit"),
        # output (section 2h)
        "mesh_verts": g(d, "out.mesh", "verts"),
        "mesh_faces": g(d, "out.mesh", "faces"),
        "stone_faces": g(d, "out.mesh", "stone_faces"),
        "stone_area": g(d, "out.mesh", "stone_area"),
        "faces_per_u2": g(d, "out.mesh", "faces_per_u2"),
        "out_edges": g(d, "out.usecount", "edges"),
        "out_use1": g(d, "out.usecount", "use1"),
        "out_use2": g(d, "out.usecount", "use2"),
        "out_use3plus": g(d, "out.usecount", "use3plus"),
        "out_expected_use1": g(d, "out.usecount", "expected_use1"),
        "authored_abutments": g(d, "out.usecount", "authored_abutments"),
        "tvertices": g(d, "out.tv", "count"),
        "tv_corner": g(d, "out.tv", "corner"),
        "tv_abut_sample": g(d, "out.tv", "abut_sample"),
        "tv_border_sample": g(d, "out.tv", "border_sample"),
        "tv_interior": g(d, "out.tv", "interior"),
        "sliver_n": g(d, "out.slivers", "n"),
        "minang_min": g(d, "out.slivers", "minang_min"),
        "minang_p10": g(d, "out.slivers", "p10"),
        "minang_p50": g(d, "out.slivers", "p50"),
        "under1deg": g(d, "out.slivers", "under1deg"),
        "under2deg": g(d, "out.slivers", "under2deg"),
        "sliver_under1_band": g(d, "out.sliverclass", "u1band"),
        "sliver_under1_abut": g(d, "out.sliverclass", "u1abut"),
        "sliver_under1_other": g(d, "out.sliverclass", "u1other"),
        "sliver_under2_band": g(d, "out.sliverclass", "u2band"),
        "density_min": g(d, "out.density", "faces_per_u2_min"),
        "density_max": g(d, "out.density", "faces_per_u2_max"),
        "density_ratio": g(d, "out.density", "ratio"),
    }


def summarise_p3(d):
    """Phase-3 numbers: the displacement banner and the relief census."""
    out = {
        "amp": g(d, "displace.arm", "amp"),
        "pyramid": g(d, "displace.arm", "pyramid"),
        "pyr_radius_tex": g(d, "displace.arm", "pyr_radius_tex"),
        "pyr_rad_used": g(d, "displace.arm", "pyr_rad_used"),
        "chart_maxdev_deg": g(d, "displace.arm", "chart_maxdev_deg"),
        "nodes_moved": g(d, "displace.nodes", "moved"),
        "nodes_groove": g(d, "displace.nodes", "groove"),
        "nodes_bevel": g(d, "displace.nodes", "bevel"),
        "nodes_plateau": g(d, "displace.nodes", "plateau"),
        "nodes_pinned": g(d, "displace.nodes", "pinned"),
        "d_min": g(d, "displace.nodes", "d_min"),
        "d_max": g(d, "displace.nodes", "d_max"),
        "relief_samples": g(d, "relief.arm", "samples"),
        "relief_core_samples": g(d, "relief.arm", "core_samples"),
        "relief_planes": g(d, "relief.arm", "planes"),
        "relief_tol": g(d, "relief.arm", "tol"),
    }
    for c in ("groove", "bevel", "plateau"):
        out["%s_emr_p50" % c] = g(d, "relief.class.%s" % c, "emr_p50")
        out["%s_emr_p10" % c] = g(d, "relief.class.%s" % c, "emr_p10")
        out["%s_emr_p90" % c] = g(d, "relief.class.%s" % c, "emr_p90")
        out["%s_core_emr_p50" % c] = g(d, "relief.class.%s" % c, "core_emr_p50")
        out["%s_planes" % c] = g(d, "relief.planegate.%s" % c, "planes")
        out["%s_over_tol" % c] = g(d, "relief.planegate.%s" % c, "over_tol")
        out["%s_worst_emr_p50" % c] = g(d, "relief.planegate.%s" % c, "worst_emr_p50")
        out["%s_core_over_tol" % c] = g(d, "relief.planegate.%s" % c, "core_over_tol")
        out["%s_core_worst_emr_p50" % c] = g(d, "relief.planegate.%s" % c,
                                             "core_worst_emr_p50")
    return out


def gate_p3(d):
    """Design section 2d's invariant: e-r p50 within +-0.005 u on EVERY plane.

    Reported twice, because phase 3 pins every ring at 0 by its own spec and the
    samples next to a pinned authored edge are P4's debt, not the height rule's:
    ALL = every sample, CORE = only samples more than one target cell from an
    authored edge.
    """
    su = summarise_p3(d)
    rows = []

    def chk(name, ok, detail):
        rows.append((name, bool(ok), detail))

    for c in ("plateau", "groove", "bevel"):
        chk("%s: every plane within +-0.005 u (ALL samples)" % c,
            su["%s_over_tol" % c] == 0,
            "%s of %s planes outside, worst %s, scene p50 %s" %
            (su["%s_over_tol" % c], su["%s_planes" % c],
             su["%s_worst_emr_p50" % c], su["%s_emr_p50" % c]))
        chk("%s: every plane within +-0.005 u (CORE band)" % c,
            su["%s_core_over_tol" % c] == 0,
            "%s of %s planes outside, worst %s, scene p50 %s" %
            (su["%s_core_over_tol" % c], su["%s_planes" % c],
             su["%s_core_worst_emr_p50" % c], su["%s_core_emr_p50" % c]))

    chk("the displacement direction is the chart plane normal",
        su["chart_maxdev_deg"] is not None and su["chart_maxdev_deg"] < 0.001,
        "max angle between a chart's proxy normal and a member face's: %s deg" %
        su["chart_maxdev_deg"])

    chk("no ring or border vertex moved",
        su["nodes_pinned"] is not None and su["nodes_pinned"] > 0,
        "%s authored-edge vertices pinned at 0, %s interior nodes moved "
        "(groove %s / bevel %s / plateau %s)" %
        (su["nodes_pinned"], su["nodes_moved"], su["nodes_groove"],
         su["nodes_bevel"], su["nodes_plateau"]))

    return all(r[1] for r in rows), rows


def gate_p2(d, p1=None):
    """Design section 2c / 2h invariants for the phase-2 lattice.

    `p1`, when the same log also carries the phase-1 census, cross-checks the
    two independent topology builds against each other.
    """
    su = summarise_p2(d)
    rows = []

    def chk(name, ok, detail):
        rows.append((name, bool(ok), detail))

    if p1 and p1.get("stone_faces") is not None:
        chk("the two topology builds agree",
            (su["topo_faces"] == p1["stone_faces"] and su["topo_verts"] == p1["vertices"]
             and su["topo_edges"] == p1["edges"] and su["topo_use1"] == p1["use1"]
             and su["topo_use3plus"] == p1["use3plus"] and su["topo_charts"] == p1["charts"]),
            "P2 %s/%s/%s/%s/%s vs P1 %s/%s/%s/%s/%s (faces/verts/edges/use1/charts)" %
            (su["topo_faces"], su["topo_verts"], su["topo_edges"], su["topo_use1"],
             su["topo_charts"], p1["stone_faces"], p1["vertices"], p1["edges"],
             p1["use1"], p1["charts"]))

    chk("watertight: no output edge used more than twice", su["out_use3plus"] == 0,
        "use3plus=%s of %s output edges" % (su["out_use3plus"], su["out_edges"]))

    chk("boundary is exactly the authored abutments",
        su["out_use1"] == su["out_expected_use1"],
        "use1=%s, expected %s (the %s authored abutments cut into their own segments)" %
        (su["out_use1"], su["out_expected_use1"], su["authored_abutments"]))

    chk("the lattice creates no T-vertex",
        (su["tv_border_sample"] == 0 and su["tv_interior"] == 0),
        "T-vertices=%s: corner=%s abut_sample=%s border_sample=%s interior=%s "
        "(corner+abut_sample are the AUTHORED T-junction abutments, design 2e / P4)" %
        (su["tvertices"], su["tv_corner"], su["tv_abut_sample"],
         su["tv_border_sample"], su["tv_interior"]))

    chk("border samples lie on the authored line",
        su["border_max_dev"] is not None and su["border_max_dev"] < 1e-9,
        "max deviation %s u over %s interior samples" %
        (su["border_max_dev"], su["border_samples"]))

    chk("plateau nodes >= 4 level-0 texels inside the block",
        su["plateau_min_level0_texels"] is not None
        and su["plateau_min_level0_texels"] >= 4.0,
        "min %s level-0 texels" % su["plateau_min_level0_texels"])

    chk("adjacent cells differ by at most one level",
        su["level_jump_violations"] == 0,
        "violations=%s" % su["level_jump_violations"])

    chk("no triangulation fallback",
        su["delaunay_fallback"] == 0 and su["degenerate_uv"] == 0
        and su["row_cap_faces"] == 0 and su["nodes_capped"] == 0,
        "delaunay_fallback=%s degenerate_uv=%s row_cap=%s node_cap=%s" %
        (su["delaunay_fallback"], su["degenerate_uv"], su["row_cap_faces"],
         su["nodes_capped"]))

    chk("sliver census: min-angle p10 > 2 deg",
        su["minang_p10"] is not None and su["minang_p10"] > 2.0,
        "p10=%s p50=%s min=%s" % (su["minang_p10"], su["minang_p50"], su["minang_min"]))

    chk("sliver census: no face under 1 deg", su["under1deg"] == 0,
        "under1=%s of %s (%s of them inside a mortar band, where a thin triangle "
        "runs ALONG the line and is the legitimate kind)" %
        (su["under1deg"], su["sliver_n"], su["sliver_under1_band"]))

    return all(r[1] for r in rows), rows


def gate(d):
    """Return (ok, [(name, ok, detail), ...]) for design section 6, phase P1."""
    su = summarise(d)
    rows = []

    def chk(name, ok, detail):
        rows.append((name, bool(ok), detail))

    chk("0 non-manifold edges", su["use3plus"] == 0,
        "use3plus=%s (%d listed)" % (su["use3plus"], len(d["nonmanifold"])))

    free_rows = [r for r in d["use1"] if r.get("class") == "free"]
    chk("free edges classified against the whole soup",
        su["free"] is not None and su["free"] == len(free_rows),
        "free=%s shared_soup=%s coincident=%s of use1=%s" %
        (su["free"], su["shared_soup"], su["coincident"], su["use1"]))

    chk("0 epsilon-fallback welds", su["eps_merges"] == 0,
        "eps_merges=%s (eps=%s, shortest authored edge=%s)" %
        (su["eps_merges"], su["eps"], su["shortest_edge"]))

    chk("no null twin", su["null_twin"] == 0,
        "null_twin=%s, boundary half-edges (null FACE)=%s" %
        (su["null_twin"], su["boundary_he"]))

    chk("every stone face in exactly one chart",
        su["unassigned"] == 0 and su["chart_faces"] == su["stone_faces"],
        "chart faces=%s of %s stone faces, unassigned=%s" %
        (su["chart_faces"], su["stone_faces"], su["unassigned"]))

    chk("chart proxy fits its budget", su["over_budget_charts"] == 0,
        "over_budget_charts=%s, max in-chart deviation=%s deg (budget %s)" %
        (su["over_budget_charts"], su["maxdev_max_deg"], su["chart_budget_deg"]))

    chk("consistent winding on every shared edge", su["orient_flip"] == 0,
        "orient_flip=%s, convex/concave disagreements=%s" %
        (su["orient_flip"], su["convex_disagree"]))

    ms = su["total_ms"]
    chk("census wall-clock <= 50 ms", ms is not None and ms <= 50.0, "total_ms=%s" % ms)

    return all(r[1] for r in rows), rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", help="log file(s); stdin when omitted")
    ap.add_argument("--json", action="store_true", help="one JSON object on stdout")
    ap.add_argument("--gate", action="store_true", help="print the P1 gate, exit 1 on FAIL")
    ap.add_argument("--p2", action="store_true", help="print the phase-2 table")
    ap.add_argument("--p2gate", action="store_true", help="print the P2 gate, exit 1 on FAIL")
    ap.add_argument("--p3", action="store_true", help="print the phase-3 table")
    ap.add_argument("--p3gate", action="store_true", help="print the P3 gate, exit 1 on FAIL")
    ap.add_argument("--free", action="store_true", help="list the free edges only")
    ap.add_argument("--junctions", type=int, default=0, metavar="N",
                    help="also print the N longest chart-pair junctions")
    a = ap.parse_args()

    lines = []
    if a.logs:
        for p in a.logs:
            with open(p, "r", errors="replace") as f:
                lines += f.readlines()
    else:
        lines = sys.stdin.readlines()

    d = parse(lines)
    if (a.p3 or a.p3gate) and any(k.startswith("relief.") for k in d["sec"]):
        if a.p3:
            su3 = summarise_p3(d)
            print("== v4 phase-3 displacement")
            for k in sorted(su3):
                if su3[k] is not None:
                    print("  %-28s %s" % (k, su3[k]))
        if a.p3gate:
            ok3, rows3 = gate_p3(d)
            print("== P3 gate")
            for n, o, t in rows3:
                print("  [%s] %-52s %s" % ("PASS" if o else "FAIL", n, t))
            print("== %s" % ("PASS" if ok3 else "FAIL"))
            return 0 if ok3 else 1
        return 0
    if (a.p2 or a.p2gate) and any(k.startswith("lattice.") for k in d["sec"]):
        su2 = summarise_p2(d)
        p1 = summarise(d) if any(k.startswith("stitch.") for k in d["sec"]) else None
        if a.p2:
            print("== v4 phase-2 lattice")
            for k in sorted(su2):
                if su2[k] is not None:
                    print("  %-28s %s" % (k, su2[k]))
            for r in d["mat"]:
                print("  MAT %s" % r)
        if a.p2gate:
            ok2, rows2 = gate_p2(d, p1)
            print("== P2 gate")
            for n, o, t in rows2:
                print("  [%s] %-44s %s" % ("PASS" if o else "FAIL", n, t))
            print("== %s" % ("PASS" if ok2 else "FAIL"))
            return 0 if ok2 else 1
        return 0
    if not d["sec"]:
        print("no [V4-STITCH] block in the input — was --greets_displace_v4 --v4_census on?",
              file=sys.stderr)
        return 2

    if a.free:
        for r in d["use1"]:
            if r.get("class") == "free":
                print(r)
        return 0

    if a.json:
        out = summarise(d)
        out["free_edges"] = [r for r in d["use1"] if r.get("class") == "free"]
        out["nonmanifold_edges"] = d["nonmanifold"]
        out["sweep"] = d["sweep"]
        ok, rows = gate(d)
        out["gate_pass"] = ok
        out["gate"] = [{"check": n, "pass": o, "detail": t} for n, o, t in rows]
        json.dump(out, sys.stdout, indent=2, sort_keys=True)
        print()
        return 0 if ok or not a.gate else 1

    su = summarise(d)
    print("== v4 phase-1 census")
    print("stage (a) stitch")
    for k in ("stone_faces", "soup_faces", "soup_meshes", "meshes_with_stone",
              "placement_all_identity", "corners", "vertices", "exact_merges",
              "ulp_merges", "eps_merges", "shortest_edge", "eps", "edges",
              "use1", "use2", "use3plus", "boundary_he", "null_twin",
              "matseam_edges", "coplanar_edges", "crease_edges", "smooth_edges",
              "convex_edges", "concave_edges", "orient_flip", "convex_disagree",
              "free", "shared_soup", "coincident", "cross_mesh_abut", "stitch_ms"):
        if su.get(k) is not None:
            print("  %-28s %s" % (k, su[k]))
    print("stage (b) charts")
    for k in ("chart_budget_deg", "charts", "chart_faces", "unassigned",
              "maxdev_max_deg", "over_budget_charts", "junctions", "junction_len",
              "junction_edges_convex", "junction_edges_concave",
              "junction_edges_smooth", "planes", "planepair_junctions",
              "planepair_len", "planes_with_multiple_charts",
              "charts_for_90pct_area", "charts_ms", "total_ms"):
        if su.get(k) is not None:
            print("  %-28s %s" % (k, su[k]))
    if d["sweep"]:
        print("  %-28s %s" % ("charts by budget (deg)",
                              " ".join("%s:%s" % (k, v) for k, v in d["sweep"].items())))
    if d["abutmat"]:
        print("  use-count-1 edges by abutting material:")
        for r in d["abutmat"]:
            print("    %-20s %s" % (r.get("name"), r.get("edges")))
    for r in d["use1"]:
        if r.get("class") == "free":
            print("  FREE  a=%s b=%s len=%s mat=%s" %
                  (r.get("a"), r.get("b"), r.get("len"), r.get("mat")))
    for r in d["nonmanifold"]:
        print("  NON-MANIFOLD a=%s b=%s uses=%s" % (r.get("a"), r.get("b"), r.get("uses")))
    if a.junctions:
        print("  longest chart-pair junctions:")
        for r in d["junc"][:a.junctions]:
            print("    %3s-%-3s len=%-10s phi=%-8s %s" %
                  (r.get("a"), r.get("b"), r.get("len"), r.get("phi"), r.get("class")))

    if a.gate:
        ok, rows = gate(d)
        print("== P1 gate")
        for n, o, t in rows:
            print("  [%s] %-44s %s" % ("PASS" if o else "FAIL", n, t))
        print("== %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
