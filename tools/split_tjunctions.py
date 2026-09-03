#!/usr/bin/env python3
"""
tools/split_tjunctions.py — Prototype T-junction splitter for REVIVAL stone meshes.

Splits host edges at T-vertices so that all adjacent faces share conforming edges.
"""

import math
import sys

def v_sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def v_add(a, b): return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
def v_scale(a, s): return (a[0]*s, a[1]*s, a[2]*s)
def v_dot(a, b): return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def v_len(a): return math.sqrt(v_dot(a, a))
def v_dist(a, b): return v_len(v_sub(a, b))

def load_obj(path):
    verts = []
    texcoords = []
    normals = []
    # faces: list of list of (v_idx, vt_idx, vn_idx) tuples (0-based)
    faces = []
    face_mats = []
    cur_mat = "default"
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'): continue
            parts = line.split()
            if parts[0] == 'v':
                verts.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif parts[0] == 'vt':
                texcoords.append((float(parts[1]), float(parts[2])))
            elif parts[0] == 'vn':
                normals.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif parts[0] == 'usemtl':
                cur_mat = parts[1]
            elif parts[0] == 'f':
                corners = []
                for p in parts[1:]:
                    vals = p.split('/')
                    vi = int(vals[0]) - 1
                    vti = int(vals[1]) - 1 if len(vals) > 1 and vals[1] else -1
                    vni = int(vals[2]) - 1 if len(vals) > 2 and vals[2] else -1
                    corners.append((vi, vti, vni))
                faces.append(corners)
                face_mats.append(cur_mat)
    return verts, texcoords, normals, faces, face_mats

def save_obj(path, verts, texcoords, normals, faces, face_mats):
    with open(path, 'w') as f:
        f.write("# REVIVAL greets stone mesh, T-junctions split\n")
        for v in verts:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for vt in texcoords:
            f.write(f"vt {vt[0]:.6f} {vt[1]:.6f}\n")
        for vn in normals:
            f.write(f"vn {vn[0]:.6f} {vn[1]:.6f} {vn[2]:.6f}\n")
        cur_mat = None
        for fi, face in enumerate(faces):
            mat = face_mats[fi]
            if mat != cur_mat:
                cur_mat = mat
                f.write(f"usemtl {cur_mat}\n")
            corner_strs = []
            for vi, vti, vni in face:
                s = str(vi + 1)
                if vti >= 0 or vni >= 0:
                    s += "/" + (str(vti + 1) if vti >= 0 else "")
                if vni >= 0:
                    s += "/" + str(vni + 1)
                corner_strs.append(s)
            f.write(f"f {' '.join(corner_strs)}\n")

def split_tjunctions(verts, texcoords, normals, faces, face_mats, eps=1e-3):
    # First, weld vertex positions to find unique geometric vertices
    unique_pos = []
    vert_to_unique = []
    for p in verts:
        found = -1
        for ui, up in enumerate(unique_pos):
            if v_dist(p, up) < eps:
                found = ui
                break
        if found < 0:
            found = len(unique_pos)
            unique_pos.append(p)
        vert_to_unique.append(found)

    # Map unique vertex back to a representative original vertex index
    unique_to_vert = {}
    for vi, ui in enumerate(vert_to_unique):
        if ui not in unique_to_vert:
            unique_to_vert[ui] = vi

    print(f"[SPLIT] {len(verts)} verts -> {len(unique_pos)} unique geometric positions, {len(faces)} faces", file=sys.stderr)

    total_splits = 0
    changed = True
    round_num = 0
    while changed and round_num < 8:
        changed = False
        round_num += 1
        new_faces = []
        new_mats = []
        
        for fi, face in enumerate(faces):
            assert len(face) == 3, "Only triangles supported"
            # Check the 3 edges of this triangle
            split_found = False
            for k in range(3):
                c0 = face[k]
                c1 = face[(k + 1) % 3]
                c2 = face[(k + 2) % 3] # opposite vertex

                p0 = verts[c0[0]]
                p1 = verts[c1[0]]
                edge_vec = v_sub(p1, p0)
                L = v_len(edge_vec)
                if L < eps:
                    continue

                # Find any unique vertex on this edge (excluding endpoints)
                best_u = -1
                best_t = -1.0
                best_d = float('inf')

                for ui, up in enumerate(unique_pos):
                    # Must not be endpoint
                    if v_dist(up, p0) < eps or v_dist(up, p1) < eps:
                        continue
                    # Parameter along edge
                    t = v_dot(v_sub(up, p0), edge_vec) / (L * L)
                    if t <= (eps / L) or t >= (1.0 - eps / L):
                        continue
                    # Distance from line segment
                    proj = v_add(p0, v_scale(edge_vec, t))
                    d = v_dist(up, proj)
                    if d < eps and d < best_d:
                        best_d = d
                        best_t = t
                        best_u = ui

                if best_u >= 0:
                    # Split face at best_u!
                    v_rep = unique_to_vert[best_u]
                    
                    # Interpolate texture coordinates along edge for the new corner
                    new_vt_idx = -1
                    if c0[1] >= 0 and c1[1] >= 0:
                        vt0 = texcoords[c0[1]]
                        vt1 = texcoords[c1[1]]
                        interp_vt = (
                            (1.0 - best_t) * vt0[0] + best_t * vt1[0],
                            (1.0 - best_t) * vt0[1] + best_t * vt1[1]
                        )
                        new_vt_idx = len(texcoords)
                        texcoords.append(interp_vt)

                    # Interpolate normal along edge if present
                    new_vn_idx = -1
                    if c0[2] >= 0 and c1[2] >= 0:
                        vn0 = normals[c0[2]]
                        vn1 = normals[c1[2]]
                        interp_vn = (
                            (1.0 - best_t) * vn0[0] + best_t * vn0[1],
                            (1.0 - best_t) * vn0[1] + best_t * vn1[1],
                            (1.0 - best_t) * vn0[2] + best_t * vn1[2]
                        )
                        lvn = v_len(interp_vn)
                        if lvn > 1e-6:
                            interp_vn = v_scale(interp_vn, 1.0 / lvn)
                        new_vn_idx = len(normals)
                        normals.append(interp_vn)

                    corner_mid = (v_rep, new_vt_idx, new_vn_idx)

                    # Create two new triangles preserving winding:
                    # Original triangle was (c0, c1, c2). Edge was c0 -> c1.
                    # Tri 1: c0 -> corner_mid -> c2
                    # Tri 2: corner_mid -> c1 -> c2
                    tri1 = [c0, corner_mid, c2]
                    tri2 = [corner_mid, c1, c2]

                    new_faces.append(tri1)
                    new_mats.append(face_mats[fi])
                    new_faces.append(tri2)
                    new_mats.append(face_mats[fi])

                    total_splits += 1
                    changed = True
                    split_found = True
                    break

            if not split_found:
                new_faces.append(face)
                new_mats.append(face_mats[fi])

        faces = new_faces
        face_mats = new_mats
        print(f"[SPLIT] Round {round_num}: total splits so far = {total_splits}, faces = {len(faces)}", file=sys.stderr)

    print(f"[SPLIT] Done after {round_num} rounds. Total splits: {total_splits}, final faces: {len(faces)}", file=sys.stderr)
    return verts, texcoords, normals, faces, face_mats

def main():
    if len(sys.argv) < 3:
        print("usage: split_tjunctions.py <input.obj> <output.obj>")
        sys.exit(1)

    in_path = sys.argv[1]
    out_path = sys.argv[2]
    verts, texcoords, normals, faces, face_mats = load_obj(in_path)
    verts, texcoords, normals, faces, face_mats = split_tjunctions(
        verts, texcoords, normals, faces, face_mats
    )
    save_obj(out_path, verts, texcoords, normals, faces, face_mats)
    print(f"[SPLIT] Saved split mesh to {out_path}", file=sys.stderr)

if __name__ == '__main__':
    main()
