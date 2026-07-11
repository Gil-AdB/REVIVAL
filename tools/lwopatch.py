#!/usr/bin/env python3
"""lwopatch — patch surface (SURF) material values in LightWave LWO1 (LWOB) files.

The write-back half of the LWO surface editor (see tools/editor_server.py):
the browser editor edits live engine Materials; saving maps those values back
into the authoring .lwo files here, then tools/lwsread regenerates the FLD.

Values are given on the ENGINE scale (what the editor shows) and converted to
the on-disk LWO encoding, writing BOTH the legacy integer chunk and its float
twin (LightWave writes both; the converter's float reader wins by file order):

  prop          engine scale        LWO chunks written
  ------------  ------------------  ---------------------------------------
  diffuse       0..1                DIFF u16 (256=100%) + VDIF f32 (0..1)
  specular      0..1                SPEC u16            + VSPC f32
  luminosity    0..1                LUMI u16            + VLUM f32
  transparency  0..100 (percent)    TRAN u16            + VTRN f32
  reflection    0..100 (percent)    REFL u16            + VRFL f32
  glossiness    raw u16 exponent    GLOS u16
  color         R,G,B 0..255        COLR 3 bytes + pad

Missing subchunks are inserted at their canonical position (the order LightWave
itself writes); existing ones are updated in place. Everything else in the file
is preserved byte-for-byte — run with no --set to verify (identity self-test).

Usage:
  lwopatch.py file.lwo --list
  lwopatch.py file.lwo --set 'rooms:specular=0.1' --set 'rooms:glossiness=64' \
              [--backup-dir DIR] [--dry-run] [-o OUT]
"""

import argparse
import datetime
import math
import os
import shutil
import struct
import sys

# Canonical SURF subchunk order (as LightWave 5.x writes them). Insertion
# places a new subchunk before the first existing one that sorts later.
CANON = ["COLR", "FLAG",
         "LUMI", "VLUM", "DIFF", "VDIF", "SPEC", "VSPC", "GLOS",
         "REFL", "VRFL", "TRAN", "VTRN",
         "RFLT", "RIMG", "RSAN", "RIND", "EDGE", "SMAN",
         "CTEX", "DTEX", "STEX", "RTEX", "TTEX", "BTEX",
         "TIMG", "TFLG", "TSIZ", "TCTR", "TFAL", "TVEL", "TCLR", "TVAL",
         "TAMP", "TFRQ", "TALP", "TWRP", "TAAS", "TOPC", "TIP0", "TFP0", "TFP1"]
CANON_POS = {c: i for i, c in enumerate(CANON)}

# prop -> (int_chunk, float_chunk, engine->fraction divisor)
VALUE_PROPS = {
    "diffuse":      ("DIFF", "VDIF", 1.0),
    "specular":     ("SPEC", "VSPC", 1.0),
    "luminosity":   ("LUMI", "VLUM", 1.0),
    "transparency": ("TRAN", "VTRN", 100.0),
    "reflection":   ("REFL", "VRFL", 100.0),
}


class Surf:
    def __init__(self, name, subchunks):
        self.name = name            # str
        self.subchunks = subchunks  # list[(id:str, body:bytes)]

    def _find(self, cid):
        for i, (c, _) in enumerate(self.subchunks):
            if c == cid:
                return i
        return -1

    def set_chunk(self, cid, body):
        """Update in place, or insert at the canonical position."""
        i = self._find(cid)
        if i >= 0:
            self.subchunks[i] = (cid, body)
            return
        pos = CANON_POS.get(cid, len(CANON))
        at = len(self.subchunks)
        for j, (c, _) in enumerate(self.subchunks):
            if CANON_POS.get(c, len(CANON)) > pos:
                at = j
                break
        self.subchunks.insert(at, (cid, body))

    # UV mapping subchunks: CTEX = projection string ("Planar Image Map"...),
    # TFLG = u2 axis/flags bitfield (1=X 2=Y 4=Z + world/pixel-blend bits),
    # TSIZ = 3×f4 world-units-per-tile. The engine bakes UVs from exactly
    # these at FLD load (Get_UV), so patching them + reconverting reproduces
    # the editor's live re-projection.
    UV_PROJ_NAMES = ["Planar Image Map", "Cylindrical Image Map",
                     "Spherical Image Map", "Cubic Image Map"]

    def set_uv_mapping(self, proj, sx, sy, sz, axis):
        name = self.UV_PROJ_NAMES[int(proj)]
        body = name.encode("latin-1") + b"\x00"
        if len(body) % 2:
            body += b"\x00"
        self.set_chunk("CTEX", body)
        self.set_chunk("TSIZ", struct.pack(">3f", float(sx), float(sy), float(sz)))
        i = self._find("TFLG")
        flags = struct.unpack(">H", self.subchunks[i][1])[0] if i >= 0 else 0
        flags = (flags & ~0x7) | (int(axis) & 0x7)
        self.set_chunk("TFLG", struct.pack(">H", flags))

    def set_prop(self, prop, value):
        if prop in VALUE_PROPS:
            ichunk, fchunk, div = VALUE_PROPS[prop]
            frac = float(value) / div
            iv = max(0, min(0xFFFF, round(frac * 256.0)))
            self.set_chunk(ichunk, struct.pack(">H", iv))
            self.set_chunk(fchunk, struct.pack(">f", frac))
        elif prop == "glossiness":
            self.set_chunk("GLOS", struct.pack(">H", max(0, min(0xFFFF, round(float(value))))))
        elif prop in ("baseR", "baseG", "baseB"):
            i = self._find("COLR")
            body = bytearray(self.subchunks[i][1] if i >= 0 else b"\xc8\xc8\xc8\x00")
            body["baseRbaseGbaseB".index(prop) // 5] = max(0, min(255, round(float(value))))
            self.set_chunk("COLR", bytes(body))
        else:
            raise ValueError(f"unknown prop '{prop}'")

    def serialize(self):
        out = self.name.encode("latin-1") + b"\x00"
        if len(out) % 2:
            out += b"\x00"
        for cid, body in self.subchunks:
            out += cid.encode("ascii") + struct.pack(">H", len(body)) + body
            if len(body) % 2:
                out += b"\x00"
        return out


class LwoFile:
    """FORM/LWOB container: SURF chunks are parsed, everything else kept raw."""

    def __init__(self, path):
        self.path = path
        data = open(path, "rb").read()
        if data[0:4] != b"FORM" or data[8:12] != b"LWOB":
            raise ValueError(f"{path}: not a FORM/LWOB (LWO1) file")
        self.chunks = []   # list[("SURF", Surf) | (id:str, raw_body:bytes)]
        off = 12
        end = 8 + struct.unpack(">I", data[4:8])[0]
        while off < end:
            cid = data[off:off + 4].decode("ascii")
            ln = struct.unpack(">I", data[off + 4:off + 8])[0]
            body = data[off + 8:off + 8 + ln]
            if cid == "SURF":
                self.chunks.append(("SURF", self._parse_surf(body)))
            else:
                self.chunks.append((cid, body))
            off += 8 + ln + (ln & 1)

    @staticmethod
    def _parse_surf(body):
        z = body.index(b"\x00")
        name = body[:z].decode("latin-1")
        p = z + 1
        if p % 2:
            p += 1
        subs = []
        while p + 6 <= len(body):
            cid = body[p:p + 4].decode("ascii")
            ln = struct.unpack(">H", body[p + 4:p + 6])[0]
            subs.append((cid, body[p + 6:p + 6 + ln]))
            p += 6 + ln + (ln & 1)
        return Surf(name, subs)

    def surfaces(self):
        return [c[1] for c in self.chunks if c[0] == "SURF"]

    def surface(self, name):
        for s in self.surfaces():
            if s.name == name:
                return s
        return None

    # ── Instance-split bake (SRFS/POLS/PNTS surgery) ──────────────────────
    # Persist the editor's runtime "split instances" (two mummies share one
    # 'momy' surface) by making the split REAL in the authoring source: the
    # polygons of every spatially-separate cluster beyond the primary are
    # reassigned to a fresh surface (SRFS append + SURF clone of the base),
    # so the regenerated FLD carries 'momy' + 'momy2' as ordinary authored
    # surfaces and nothing at runtime needs to re-split.
    #
    # The clustering REPLICATES Editor_SplitInstances (DEMO/MaterialEditor
    # .cpp) at the LWO polygon level: grid single-linkage on poly centroids,
    # cell size R = 15% of the union-bbox diagonal, primary = biggest cluster
    # (tie -> the cluster of the earliest poly), other clusters numbered 2..
    # in first-seen poly order. Since the FLD converter preserves polygon
    # order and rigid transforms don't change cluster structure, cluster k
    # here corresponds to the runtime part "<name>#k" (verified empirically
    # via FOCUS_TEST centroids). Only single-mesh instances split this way —
    # LWS-instanced copies (two LoadObject lines of one file) present a
    # single spatial cluster here and return None (live-only split remains).

    def _raw_chunk(self, cid):
        for i, (c, body) in enumerate(self.chunks):
            if c == cid and c != "SURF":
                return i, body
        return -1, None

    def srfs_names(self):
        """SRFS surface names in file order (1-based POLS indices)."""
        _, body = self._raw_chunk("SRFS")
        if body is None:
            return []
        names, p = [], 0
        while p < len(body):
            z = body.index(b"\x00", p)
            names.append(body[p:z].decode("latin-1"))
            p = z + 1
            if p % 2:
                p += 1
        return names

    def points(self):
        """PNTS as [(x,y,z)] in raw LWO coordinates (no YZ swap — clustering
        only needs relative positions, which are swap-invariant)."""
        _, body = self._raw_chunk("PNTS")
        if body is None:
            return []
        n = len(body) // 12
        flat = struct.unpack(f">{n * 3}f", body[:n * 12])
        return [(flat[i * 3], flat[i * 3 + 1], flat[i * 3 + 2]) for i in range(n)]

    def polys(self):
        """POLS as [(vert_indices, surf_1based, surf_field_offset)] — the
        offset indexes the u16 surface field inside the POLS body so
        split_surface can rewrite it in place. Detail polygons (negative
        surface) are rejected, same as the converter."""
        _, body = self._raw_chunk("POLS")
        if body is None:
            return []
        out, p = [], 0
        while p + 4 <= len(body):
            (nv,) = struct.unpack_from(">H", body, p)
            p += 2
            verts = struct.unpack_from(f">{nv}H", body, p)
            p += 2 * nv
            (surf,) = struct.unpack_from(">h", body, p)
            if surf < 0:
                raise ValueError("detail polygons are not supported")
            out.append((verts, surf, p))
            p += 2
        return out

    @staticmethod
    def _cluster(cents):
        """Grid single-linkage union-find, replicating Editor_SplitInstances:
        returns (roots_per_index, R). Union direction matches the C++
        (parent[find(i)] = find(cell_rep)) so tie-breaks agree."""
        n = len(cents)
        lo = [min(c[a] for c in cents) for a in range(3)]
        hi = [max(c[a] for c in cents) for a in range(3)]
        diag = math.sqrt(sum((hi[a] - lo[a]) ** 2 for a in range(3)))
        R = max(diag * 0.15, 1e-6)
        parent = list(range(n))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        cell = {}
        for i, c in enumerate(cents):
            g = (math.floor(c[0] / R), math.floor(c[1] / R), math.floor(c[2] / R))
            for ox in (-1, 0, 1):
                for oy in (-1, 0, 1):
                    for oz in (-1, 0, 1):
                        r = cell.get((g[0] + ox, g[1] + oy, g[2] + oz))
                        if r is not None:
                            a, b = find(r), find(i)
                            if a != b:
                                parent[b] = a
            cell[g] = find(i)
        return [find(i) for i in range(n)], R

    def analyze_split(self, name):
        """Cluster surface `name`'s polygons WITHOUT touching the file.
        Returns None when there is nothing to split (surface missing /
        <2 polys / one spatial cluster — e.g. LWS-instanced copies), else
          {"clusters": [{"polys": n, "centroid": (x,y,z)} ...],  # first-seen order
           "radius": R,
           ...private keys for commit_split...}
        Centroids are RAW LWO coordinates. Note the FLD converter's SwapYZ is
        a no-op, so for identity-motion objects these ARE engine world
        coordinates (plus the object's keyframe-0 offset)."""
        srfs = self.srfs_names()
        if name not in srfs:
            return None
        si = srfs.index(name) + 1          # POLS surface indices are 1-based
        pts = self.points()
        mine = [(verts, off) for (verts, surf, off) in self.polys() if surf == si]
        if len(mine) < 2:
            return None
        cents = []
        for verts, _ in mine:
            xs = [pts[v] for v in verts if v < len(pts)]
            m = len(xs) or 1
            cents.append((sum(p[0] for p in xs) / m,
                          sum(p[1] for p in xs) / m,
                          sum(p[2] for p in xs) / m))
        roots, R = self._cluster(cents)
        order = []                          # first-seen cluster order
        seen = {}
        for r in roots:
            if r not in seen:
                seen[r] = len(order)
                order.append(r)
        if len(order) < 2:
            return None
        clusters = []
        for r in order:
            idxs = [i for i, rr in enumerate(roots) if rr == r]
            cx = sum(cents[i][0] for i in idxs) / len(idxs)
            cy = sum(cents[i][1] for i in idxs) / len(idxs)
            cz = sum(cents[i][2] for i in idxs) / len(idxs)
            clusters.append({"polys": len(idxs), "centroid": (cx, cy, cz)})
        return {"clusters": clusters, "radius": R,
                "_name": name, "_srfs": srfs, "_mine": mine,
                "_roots": roots, "_order": order}

    def default_split_parts(self, analysis):
        """The order-based part numbering Editor_SplitInstances uses when no
        geometric matching is available: part 1 (keeps the base name) = the
        biggest cluster (tie -> earliest), parts 2.. in first-seen order.
        Returns {cluster_index: part_number}."""
        clusters = analysis["clusters"]
        primary = max(range(len(clusters)),
                      key=lambda i: (clusters[i]["polys"], -i))
        parts = {primary: 1}
        k = 2
        for i in range(len(clusters)):
            if i != primary:
                parts[i] = k
                k += 1
        return parts

    def commit_split(self, analysis, parts_by_cluster):
        """Apply the split: cluster with part number 1 keeps the base name;
        every other cluster's polygons move to a fresh REAL surface named
        '<base><k>' ('momy2' — clean authored names, no '#k'), SRFS-appended
        with a SURF clone of the base. parts_by_cluster maps cluster index
        (analyze_split order) -> part number. Returns
          {"parts": {k: surface-name}, "polys": {k: count},
           "centroids": {k: (x,y,z)}, "radius": R}."""
        name = analysis["_name"]
        srfs = analysis["_srfs"]
        mine = analysis["_mine"]
        roots = analysis["_roots"]
        order = analysis["_order"]
        clusters = analysis["clusters"]
        primary_ci = next(ci for ci, k in parts_by_cluster.items() if k == 1)
        # Fresh names, collide-avoided against SRFS.
        taken = set(srfs)
        parts = {1: name}
        polys_per = {1: clusters[primary_ci]["polys"]}
        centroids = {1: clusters[primary_ci]["centroid"]}
        new_names = {}                      # part k -> new surface name
        for ci, k in sorted(parts_by_cluster.items(), key=lambda kv: kv[1]):
            if k == 1:
                continue
            cand = f"{name}{k}"
            n2 = 2
            while cand in taken:
                cand = f"{name}{k}_{n2}"
                n2 += 1
            taken.add(cand)
            new_names[k] = cand
            parts[k] = cand
            polys_per[k] = clusters[ci]["polys"]
            centroids[k] = clusters[ci]["centroid"]
        # SRFS: append the new names (each NUL-terminated, padded to even) —
        # existing bytes untouched.
        srfs_i, srfs_body = self._raw_chunk("SRFS")
        add = b""
        for k in sorted(new_names):
            nb = new_names[k].encode("latin-1") + b"\x00"
            if len(nb) % 2:
                nb += b"\x00"
            add += nb
        self.chunks[srfs_i] = ("SRFS", srfs_body + add)
        # SURF: clone the base surface's subchunks under each new name and
        # append at the end of the file (converters match SURF by name).
        base_surf = self.surface(name)
        for k in sorted(new_names):
            self.chunks.append(("SURF", Surf(new_names[k],
                                             list(base_surf.subchunks))))
        # POLS: reassign every non-primary cluster poly to its new surface
        # index (1-based position in the extended SRFS).
        pols_i, pols_body = self._raw_chunk("POLS")
        body = bytearray(pols_body)
        new_index = {k: len(srfs) + i + 1
                     for i, k in enumerate(sorted(new_names))}
        root_part = {order[ci]: k for ci, k in parts_by_cluster.items()}
        for (verts, off), root in zip(mine, roots):
            k = root_part[root]
            if k == 1:
                continue
            struct.pack_into(">h", body, off, new_index[k])
        self.chunks[pols_i] = ("POLS", bytes(body))
        return {"parts": parts, "polys": polys_per,
                "centroids": centroids, "radius": analysis["radius"]}

    def split_surface(self, name):
        """analyze + commit with the default order-based part numbering (the
        no-live-centroids path; the editor server matches clusters to the
        live '#k' parts geometrically and calls commit_split itself)."""
        analysis = self.analyze_split(name)
        if analysis is None:
            return None
        return self.commit_split(analysis, self.default_split_parts(analysis))

    def serialize(self):
        out = b""
        for cid, c in self.chunks:
            body = c.serialize() if cid == "SURF" else c
            out += cid.encode("ascii") + struct.pack(">I", len(body)) + body
            if len(body) % 2:
                out += b"\x00"
        return b"FORM" + struct.pack(">I", len(out) + 4) + b"LWOB" + out


def backup(path, backup_dir):
    os.makedirs(backup_dir, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    dst = os.path.join(backup_dir, f"{stem}.{ts}{ext}")
    n = 0
    while os.path.exists(dst):   # same-second saves
        n += 1
        dst = os.path.join(backup_dir, f"{stem}.{ts}-{n}{ext}")
    shutil.copy2(path, dst)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file")
    ap.add_argument("--set", action="append", default=[],
                    metavar="SURF:PROP=VALUE",
                    help="e.g. 'rooms:specular=0.1' (engine scale; repeatable)")
    ap.add_argument("--list", action="store_true", help="list surfaces + values")
    ap.add_argument("--split", metavar="SURF",
                    help="bake the spatial instance-split of SURF into the file "
                         "(new real surfaces '<SURF>2', ... — see split_surface)")
    ap.add_argument("--backup-dir", help="copy the original here before writing")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("-o", "--out", help="write to OUT instead of in place")
    args = ap.parse_args()

    lwo = LwoFile(args.file)
    original = open(args.file, "rb").read()

    # Identity self-test: reserialization with no edits must be byte-exact,
    # otherwise our understanding of the container is wrong — refuse to write.
    if lwo.serialize() != original:
        sys.exit(f"{args.file}: identity reserialize differs — refusing to patch")

    if args.list:
        for s in lwo.surfaces():
            vals = {c: b.hex() for c, b in s.subchunks
                    if c in ("COLR", "DIFF", "VDIF", "SPEC", "VSPC", "GLOS",
                             "LUMI", "VLUM", "TRAN", "VTRN", "REFL", "VRFL")}
            print(f"{s.name}: {vals}")
        return

    touched = False
    if args.split:
        res = lwo.split_surface(args.split)
        if res is None:
            sys.exit(f"{args.file}: '{args.split}' has nothing to split "
                     "(missing surface, <2 polys, or one spatial cluster)")
        print(f"split '{args.split}': parts={res['parts']} polys={res['polys']} "
              f"R={res['radius']:.3f}")
        for k, c in sorted(res["centroids"].items()):
            print(f"  part {k} ('{res['parts'][k]}') centroid raw-LWO "
                  f"({c[0]:.2f} {c[1]:.2f} {c[2]:.2f})")
        touched = True
    for spec in args.set:
        surf_name, _, kv = spec.rpartition(":")
        prop, _, value = kv.partition("=")
        if not surf_name or not value:
            sys.exit(f"bad --set '{spec}' (want SURF:PROP=VALUE)")
        s = lwo.surface(surf_name)
        if s is None:
            sys.exit(f"{args.file}: no surface '{surf_name}' "
                     f"(has: {', '.join(x.name for x in lwo.surfaces())})")
        s.set_prop(prop, value)
        touched = True

    out = lwo.serialize()
    dest = args.out or args.file
    if args.dry_run:
        print(f"dry-run: would write {len(out)} bytes to {dest}"
              f" ({'changed' if out != original else 'IDENTICAL'})")
        return
    if not touched and dest == args.file:
        print(f"{args.file}: identity OK ({len(out)} bytes), nothing to write")
        return
    if args.backup_dir and dest == args.file:
        print(f"backup: {backup(args.file, args.backup_dir)}")
    with open(dest, "wb") as f:
        f.write(out)
    print(f"wrote {dest} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
