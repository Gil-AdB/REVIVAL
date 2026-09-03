#!/usr/bin/env python3
"""
tools/seam_audit.py — T1 seam-consistency audit for the REVIVAL greets stone
displacement bake.

Loads the AUTHORED stone mesh and the BAKED (tessellated + displaced) stone
mesh, both as Wavefront OBJ files from --greets_mesh_dump, and checks whether
the baked mesh is seam-continuous at every authored stone edge.

The approach:
 1. Parse the authored mesh and find all interior edges (edges shared by 2
    faces, either by index or by coincident position).
 2. Parse the baked mesh and find all BOUNDARY edges (half-edges used by
    exactly one face).  These are the open seams in the baked mesh.
 3. For each authored interior edge, collect baked boundary vertices that lie
    geometrically near it (within --edge-band perpendicular distance and
    within the segment's parametric extent).
 4. Split those boundary vertices into two sides by which authored face they
    are nearer to (using face normal dot product to distinguish).
 5. Compute symmetric Hausdorff distance between the two boundary polylines.
 6. Report T-junctions: authored vertices that appear in one side but not both.

Usage:
  python3 tools/seam_audit.py <authored.obj> <baked.obj> [--tol=1e-4]
      [--edge-band=0.2] [--verbose]
"""

import argparse
import math
import sys
from collections import defaultdict


# ── vector math ──────────────────────────────────────────────────────────────
def v_add(a, b): return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
def v_sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def v_dot(a, b): return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def v_cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def v_len(a): return math.sqrt(v_dot(a, a))
def v_scale(a, s): return (a[0]*s, a[1]*s, a[2]*s)
def v_dist(a, b): return v_len(v_sub(a, b))
def v_normalize(a):
    n = v_len(a)
    return v_scale(a, 1.0/n) if n > 1e-12 else (0.0, 0.0, 0.0)


def point_to_segment(p, a, b):
    """Returns (distance, parameter t in [0,1]) of closest point on segment ab to p."""
    ab = v_sub(b, a)
    l2 = v_dot(ab, ab)
    if l2 < 1e-24:
        return v_dist(p, a), 0.0
    t = max(0.0, min(1.0, v_dot(v_sub(p, a), ab) / l2))
    proj = v_add(a, v_scale(ab, t))
    return v_dist(p, proj), t


def point_to_line(p, a, b):
    """Returns (distance, parameter t along INFINITE line a->b)."""
    ab = v_sub(b, a)
    l2 = v_dot(ab, ab)
    if l2 < 1e-24:
        return v_dist(p, a), 0.0
    t = v_dot(v_sub(p, a), ab) / l2
    proj = v_add(a, v_scale(ab, t))
    return v_dist(p, proj), t


def point_to_polyline_dist(p, polyline):
    """Min distance from p to the polyline (sequence of segments)."""
    best = float('inf')
    if len(polyline) < 2:
        if len(polyline) == 1:
            return v_dist(p, polyline[0])
        return best
    for i in range(len(polyline) - 1):
        d, _ = point_to_segment(p, polyline[i], polyline[i+1])
        if d < best:
            best = d
    return best


def symmetric_hausdorff(A, B):
    """Symmetric Hausdorff between two polylines (point-to-polyline)."""
    if not A or not B:
        return float('inf')
    h_ab = max(point_to_polyline_dist(p, B) for p in A)
    h_ba = max(point_to_polyline_dist(p, A) for p in B)
    return max(h_ab, h_ba)


def face_normal(pts):
    """Newell's method for polygon normal."""
    n = [0.0, 0.0, 0.0]
    for i in range(len(pts)):
        p1 = pts[i]
        p2 = pts[(i+1) % len(pts)]
        n[0] += (p1[1]-p2[1]) * (p1[2]+p2[2])
        n[1] += (p1[2]-p2[2]) * (p1[0]+p2[0])
        n[2] += (p1[0]-p2[0]) * (p1[1]+p2[1])
    return v_normalize(tuple(n))


def face_centroid(verts, face):
    pts = [verts[vi] for vi in face['vi']]
    if not pts:
        return (0.0, 0.0, 0.0)
    sx = sum(p[0] for p in pts) / len(pts)
    sy = sum(p[1] for p in pts) / len(pts)
    sz = sum(p[2] for p in pts) / len(pts)
    return (sx, sy, sz)


# ── OBJ loader ──────────────────────────────────────────────────────────────
def load_obj(path):
    """Returns (vertices, faces).
    vertices: list of (x,y,z) tuples
    faces: list of {'vi': [vertex-index, ...], 'mat': str}
    """
    verts = []
    faces = []
    cur_mat = 'default'
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] == '#':
                continue
            parts = line.split()
            if parts[0] == 'v':
                verts.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif parts[0] == 'usemtl':
                m = parts[1]
                # rooms::mirUV is a bitangent-handedness clone of rooms
                if m == 'rooms::mirUV':
                    m = 'rooms'
                cur_mat = m
            elif parts[0] == 'f':
                vis = []
                for tok in parts[1:]:
                    idx = int(tok.split('/')[0])
                    if idx < 0:
                        idx += len(verts)
                    else:
                        idx -= 1  # OBJ is 1-based
                    vis.append(idx)
                faces.append({'vi': vis, 'mat': cur_mat})
    return verts, faces


# ── spatial hashing ─────────────────────────────────────────────────────────
def pos_key(p, tol):
    """Quantise a position for hashing with tolerance `tol`."""
    return (round(p[0]/tol), round(p[1]/tol), round(p[2]/tol))


def edge_key_pos(a, b, tol):
    """Canonical position-based edge key."""
    ka, kb = pos_key(a, tol), pos_key(b, tol)
    return (ka, kb) if ka <= kb else (kb, ka)


# ── main ────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description='T1 seam-consistency audit for the REVIVAL stone displacement bake.')
    parser.add_argument('authored', help='Path to authored OBJ (before the bake)')
    parser.add_argument('baked', help='Path to baked OBJ (after the bake)')
    parser.add_argument('--tol', type=float, default=1e-4,
                        help='Position coincidence epsilon (default 1e-4)')
    parser.add_argument('--edge-band', type=float, default=0.2,
                        help='Max perpendicular distance from authored edge '
                             'for boundary vertex collection (default 0.2 u)')
    parser.add_argument('--verbose', action='store_true',
                        help='Print per-face assignment diagnostics')
    args = parser.parse_args()
    tol = args.tol
    band = args.edge_band

    # ── Phase 1: load authored mesh, find interior edges ─────────────────
    a_verts, a_faces = load_obj(args.authored)
    print(f'Authored mesh: {args.authored} ({len(a_faces)} faces, '
          f'{len(a_verts)} vertices)', file=sys.stderr)

    # Build position-based edge -> face list for the authored mesh.
    a_edge_faces = defaultdict(list)   # edge_key -> [(face_idx, p1, p2), ...]
    for fi, face in enumerate(a_faces):
        pts = [a_verts[vi] for vi in face['vi']]
        n = len(pts)
        for i in range(n):
            p1, p2 = pts[i], pts[(i+1) % n]
            ek = edge_key_pos(p1, p2, tol)
            a_edge_faces[ek].append((fi, p1, p2))

    # Interior edges: used by exactly 2 faces (by position, ignoring index
    # splitting from the mirUV clone split).
    interior_edges = []
    for ek, entries in a_edge_faces.items():
        if len(entries) == 2:
            interior_edges.append(entries)
        elif len(entries) > 2:
            print(f'  WARNING: non-manifold authored edge at '
                  f'{entries[0][1]} -- {len(entries)} faces',
                  file=sys.stderr)

    open_edges = sum(1 for e in a_edge_faces.values() if len(e) == 1)
    print(f'  Interior edges: {len(interior_edges)}, '
          f'open borders: {open_edges}', file=sys.stderr)

    # Also find all authored vertices that lie strictly inside another
    # authored edge (T-junctions in the authored topology).
    authored_t_junctions = []
    for vi, v in enumerate(a_verts):
        for ek, entries in a_edge_faces.items():
            p1, p2 = entries[0][1], entries[0][2]
            d, t = point_to_line(v, p1, p2)
            if d < tol and 0.01 < t < 0.99:
                # Check this vertex is NOT an endpoint of the edge
                if v_dist(v, p1) > tol and v_dist(v, p2) > tol:
                    authored_t_junctions.append({
                        'vi': vi, 'pos': v, 't': t,
                        'edge_p1': p1, 'edge_p2': p2
                    })
                    break  # only report once per vertex

    print(f'  Authored T-junction vertices: {len(authored_t_junctions)}',
          file=sys.stderr)
    for tj in authored_t_junctions:
        p = tj['pos']
        print(f'    T-vert ({p[0]:.3f},{p[1]:.3f},{p[2]:.3f}) at t={tj["t"]:.3f} '
              f'along ({tj["edge_p1"][0]:.3f},{tj["edge_p1"][1]:.3f},{tj["edge_p1"][2]:.3f})'
              f'--({tj["edge_p2"][0]:.3f},{tj["edge_p2"][1]:.3f},{tj["edge_p2"][2]:.3f})',
              file=sys.stderr)

    # ── Phase 2: load baked mesh, find boundary edges ────────────────────
    b_verts, b_faces = load_obj(args.baked)
    print(f'Baked mesh: {args.baked} ({len(b_faces)} faces, '
          f'{len(b_verts)} vertices)', file=sys.stderr)

    # Store baked face normals.
    b_face_normals = []
    for face in b_faces:
        pts = [b_verts[vi] for vi in face['vi']]
        b_face_normals.append(face_normal(pts))

    # Build baked-mesh edge -> face-list (by position).
    b_edge_faces = defaultdict(list)
    for fi, face in enumerate(b_faces):
        vis = face['vi']
        n = len(vis)
        for i in range(n):
            p1, p2 = b_verts[vis[i]], b_verts[vis[(i+1) % n]]
            ek = edge_key_pos(p1, p2, tol)
            b_edge_faces[ek].append((fi, p1, p2))

    # Boundary edges: used by exactly 1 face in the baked mesh.
    boundary_verts_set = set()  # position keys of boundary vertices
    boundary_vert_pos = {}      # pos_key -> position
    boundary_vert_faces = defaultdict(set)  # pos_key -> set of baked face indices

    for ek, entries in b_edge_faces.items():
        if len(entries) == 1:
            fi, p1, p2 = entries[0]
            for p in (p1, p2):
                pk = pos_key(p, tol)
                boundary_verts_set.add(pk)
                boundary_vert_pos[pk] = p
                boundary_vert_faces[pk].add(fi)

    print(f'  Baked boundary vertices: {len(boundary_verts_set)}',
          file=sys.stderr)

    # ── Phase 3: build a spatial index of baked boundary verts ──────────
    cell_size = max(band, 0.5)
    grid = defaultdict(list)
    for pk in boundary_verts_set:
        p = boundary_vert_pos[pk]
        cx, cy, cz = int(p[0]//cell_size), int(p[1]//cell_size), int(p[2]//cell_size)
        grid[(cx, cy, cz)].append(pk)

    def query_near_segment(seg_a, seg_b, max_dist, t_lo=-0.05, t_hi=1.05):
        """Return boundary vertex pos-keys near the authored segment."""
        lo = [min(seg_a[i], seg_b[i]) - max_dist for i in range(3)]
        hi = [max(seg_a[i], seg_b[i]) + max_dist for i in range(3)]
        c_lo = [int(lo[i]//cell_size) for i in range(3)]
        c_hi = [int(hi[i]//cell_size) for i in range(3)]
        result = []
        for cx in range(c_lo[0], c_hi[0]+1):
            for cy in range(c_lo[1], c_hi[1]+1):
                for cz in range(c_lo[2], c_hi[2]+1):
                    for pk in grid.get((cx, cy, cz), []):
                        p = boundary_vert_pos[pk]
                        d, t = point_to_line(p, seg_a, seg_b)
                        if d <= max_dist and t_lo <= t <= t_hi:
                            result.append((pk, p, t, d))
        return result

    # Precompute authored face normals
    a_face_normals = []
    for face in a_faces:
        pts = [a_verts[vi] for vi in face['vi']]
        a_face_normals.append(face_normal(pts))

    # ── Phase 4: per-authored-edge audit ─────────────────────────────────
    results = []
    max_h = 0.0
    max_h_edge = None
    total_fails = 0
    total_t_juncs = 0

    for entry_pair in interior_edges:
        (f1, p1_a, p2_a) = entry_pair[0]
        (f2, p1_b, p2_b) = entry_pair[1]
        # Use the first entry's endpoints as the canonical authored edge.
        pA, pB = p1_a, p2_a
        seg_len = v_dist(pA, pB)
        if seg_len < 1e-6:
            continue
        mid = v_scale(v_add(pA, pB), 0.5)
        mat1 = a_faces[f1]['mat']
        mat2 = a_faces[f2]['mat']

        # Dihedral angle
        n1 = a_face_normals[f1]
        n2 = a_face_normals[f2]
        cos_d = max(-1.0, min(1.0, v_dot(n1, n2)))
        dihedral_deg = math.degrees(math.acos(cos_d))

        # Collect baked boundary verts near this authored edge
        nearby = query_near_segment(pA, pB, band)
        if not nearby:
            # No boundary verts near this edge -> baked mesh is continuous here
            results.append({
                'mid': mid, 'len': seg_len, 'mat1': mat1, 'mat2': mat2,
                'dih': dihedral_deg, 'n1': 0, 'n2': 0,
                'h': 0.0, 't_juncs': 0, 'note': 'no boundary (continuous)',
            })
            continue

        # Split boundary verts into side-1 (face f1) and side-2 (face f2).
        #
        # TWO STRATEGIES depending on dihedral:
        #  - High angle (> 15 deg): the baked face normal distinguishes sides
        #  - Low angle (coplanar): the normals are nearly identical, so we use
        #    the perpendicular direction from the edge toward each authored
        #    face's centroid to define "side 1" vs "side 2".
        #
        # Compute the centroid-based side direction (works for all angles):
        edge_dir = v_normalize(v_sub(pB, pA))
        c1 = face_centroid(a_verts, a_faces[f1])
        c2 = face_centroid(a_verts, a_faces[f2])
        # Project centroids onto the plane perpendicular to the edge direction,
        # passing through pA.
        c1_rel = v_sub(c1, pA)
        c2_rel = v_sub(c2, pA)
        c1_perp = v_sub(c1_rel, v_scale(edge_dir, v_dot(c1_rel, edge_dir)))
        c2_perp = v_sub(c2_rel, v_scale(edge_dir, v_dot(c2_rel, edge_dir)))
        # "side1 direction" = perpendicular toward f1's centroid
        side1_dir = v_normalize(c1_perp)
        side2_dir = v_normalize(c2_perp)

        use_centroid_split = dihedral_deg < 15.0 or v_len(c1_perp) < 1e-6 or v_len(c2_perp) < 1e-6

        side1 = []  # list of (t, position)
        side2 = []
        for pk, pos, t, perp_dist in nearby:
            if use_centroid_split or abs(v_dot(n1, n2)) > 0.97:
                # Centroid-based split: project boundary vert perpendicular to
                # the edge, see which authored face centroid it's closer to.
                p_rel = v_sub(pos, pA)
                p_perp = v_sub(p_rel, v_scale(edge_dir, v_dot(p_rel, edge_dir)))
                d1 = v_dot(p_perp, side1_dir)
                d2 = v_dot(p_perp, side2_dir)
                if d1 >= d2:
                    side1.append((t, pos))
                else:
                    side2.append((t, pos))
            else:
                # Normal-based split: use baked face normals.
                best_dot1 = -2.0
                best_dot2 = -2.0
                for bfi in boundary_vert_faces[pk]:
                    bn = b_face_normals[bfi]
                    d1 = v_dot(bn, n1)
                    d2 = v_dot(bn, n2)
                    if d1 > best_dot1:
                        best_dot1 = d1
                    if d2 > best_dot2:
                        best_dot2 = d2
                if best_dot1 >= best_dot2:
                    side1.append((t, pos))
                else:
                    side2.append((t, pos))

        # Sort by parameter and deduplicate
        def make_polyline(pairs):
            pairs.sort(key=lambda x: x[0])
            pts = []
            for _, p in pairs:
                if not pts or v_dist(pts[-1], p) > tol:
                    pts.append(p)
            return pts

        poly1 = make_polyline(side1)
        poly2 = make_polyline(side2)

        # Hausdorff
        if poly1 and poly2:
            h = symmetric_hausdorff(poly1, poly2)
        elif poly1 or poly2:
            h = float('inf')  # one side has boundary, other doesn't -> gap
        else:
            h = 0.0

        # T-junction count: for each authored vertex on this edge (endpoints
        # + any T-junction vertex that lies on it), check if it appears in
        # both sides' polylines.
        edge_author_verts = [pA, pB]
        for tj in authored_t_junctions:
            d_to_line, t_on_line = point_to_line(tj['pos'], pA, pB)
            if d_to_line < tol and -0.01 < t_on_line < 1.01:
                edge_author_verts.append(tj['pos'])

        t_juncs = 0
        for av in edge_author_verts:
            in1 = any(v_dist(av, p) < 0.05 for p in poly1) if poly1 else False
            in2 = any(v_dist(av, p) < 0.05 for p in poly2) if poly2 else False
            if in1 != in2:
                t_juncs += 1

        results.append({
            'mid': mid, 'len': seg_len, 'mat1': mat1, 'mat2': mat2,
            'dih': dihedral_deg, 'n1': len(poly1), 'n2': len(poly2),
            'h': h, 't_juncs': t_juncs, 'note': '',
        })

        if h > max_h and h < float('inf'):
            max_h = h
            max_h_edge = mid
        if h > 1e-5:
            total_fails += 1
        total_t_juncs += t_juncs

    # Sort by Hausdorff, descending (inf first, then finite descending)
    results.sort(key=lambda r: (0 if r['h'] == float('inf') else 1, -r['h']))

    # ── Phase 5: report ──────────────────────────────────────────────────
    print('=== SEAM AUDIT ===')
    print(f'Authored mesh: {args.authored} ({len(a_faces)} faces, '
          f'{len(a_verts)} vertices)')
    print(f'Baked mesh:    {args.baked} ({len(b_faces)} faces, '
          f'{len(b_verts)} vertices)')
    print(f'Authored T-junctions: {len(authored_t_junctions)}')
    print()

    # Only print edges with nonzero Hausdorff or T-junctions by default
    shown = 0
    for r in results:
        if not args.verbose and r['h'] < 1e-5 and r['t_juncs'] == 0:
            continue
        status = 'FAIL' if r['h'] > 1e-5 else 'PASS'
        m = r['mid']
        note = f'  ({r["note"]})' if r['note'] else ''
        print(f'  edge mid=({m[0]:.3f},{m[1]:.3f},{m[2]:.3f}) '
              f'len={r["len"]:.3f}  {r["mat1"]}|{r["mat2"]}  '
              f'dihedral={r["dih"]:.1f}deg')
        print(f'    side1: {r["n1"]} verts   side2: {r["n2"]} verts')
        h_str = f'{r["h"]:.6f}' if r['h'] < float('inf') else 'INF'
        print(f'    Hausdorff: {h_str} u   T-junctions: {r["t_juncs"]}  '
              f'[{status}]{note}')
        print()
        shown += 1

    suppressed = len(results) - shown
    if suppressed > 0:
        print(f'  ({suppressed} passing edges suppressed; use --verbose to show all)')
        print()

    max_h_str = f'{max_h:.6f}' if max_h_edge else '0.000000'
    max_h_pos = (f'({max_h_edge[0]:.3f},{max_h_edge[1]:.3f},{max_h_edge[2]:.3f})'
                 if max_h_edge else 'N/A')
    inf_count = sum(1 for r in results if r['h'] == float('inf'))

    print('Scene-wide:')
    print(f'  Interior edges:        {len(interior_edges)}')
    print(f'  Max finite Hausdorff:  {max_h_str} u  (at mid={max_h_pos})')
    print(f'  Edges with H > 0:      {total_fails}')
    print(f'  Edges with H = INF:    {inf_count}')
    print(f'  T-junction vertices:   {total_t_juncs}')
    overall = 'PASS' if total_fails == 0 else 'FAIL'
    print(f'  RESULT: {overall}')


if __name__ == '__main__':
    main()
