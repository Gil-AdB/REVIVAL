#!/usr/bin/env python3
"""editor_server — dev server for the browser LWO surface editor.

Serves the wasm build with the COOP/COEP headers SharedArrayBuffer needs, plus
the write-back API that persists editor changes into the LightWave sources:

  GET  /SCENES/GREETS.FLD    the LIVE Runtime/SCENES/GREETS.FLD (not the copy
                             baked into DEMO.data at link time) — the editor
                             fetches this at boot, so a browser reload picks up
                             a freshly regenerated scene without relinking.
  POST /api/greets/save      body {"surfaces": {"<name>": {"<prop>": value}}}
                             (engine-scale values, editor surface names).
                             Maps names to .lwo files, patches the SURF chunks
                             (tools/lwopatch.py), backs originals up into
                             Authoring/greets/.backups/, reruns tools/lwsread,
                             installs the new FLD into Runtime/SCENES/.

Surface-name mapping (editor -> LWO):
  "rooms"                  plain FLD name: every .lwo containing that surface
  "Hull.lwo::hull"         robot-material clone: file Hull.lwo, surface hull
  "Hull.lwo::hull_body"    per-bin clone: same, bin suffix stripped (conflicting
                           _body/_upper edits: last one wins, warned)
  "floor::mirUV"           engine handedness clone: suffix stripped

Usage:  tools/editor_server.py [--port 8099]
Then:   http://localhost:<port>/DEMO.html?editor
"""

import argparse
import base64
import hashlib
import http.server
import io
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse

try:                     # optional (thumbnails + tif/jpg -> png conversion for
    from PIL import Image  # the PBR-set install flow); the server runs without it
except ImportError:
    Image = None

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lwopatch  # noqa: E402
import fldpatch  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_ROOT = os.path.join(REPO, "build-wasm", "DEMO")
RUNTIME = os.path.join(REPO, "Runtime")
PBR_DIR = os.path.join(RUNTIME, "TEXTURES", "PBR")
LWSREAD = os.path.join(REPO, "tools", "lwsread", "build", "lwsread")
LWSREAD_LEGACY = os.path.join(REPO, "tools", "lwsread", "build", "lwsread_legacy")

# Scene registry. Scenes with pinned LWO/LWS authoring sources (Authoring/
# README status table): numeric surface edits patch the .lwo files, light
# edits patch the .lws, then the FLD is regenerated + installed. `legacy`
# selects the converter: the shipping CHASE/FOUNTAIN FLDs predate the LWO
# luminosity-unit fix (b441da6, VLUM*100), so their regen must use
# lwsread_legacy or every emissive shifts x100. Unpinned scenes patch the
# shipping FLD binary directly via tools/fldpatch.py (backups + restore under
# Runtime/SCENES/.backups/). PBR maps go to the sidecar either way.
SCENES = {
    "greets":   {"authoring": True,  "dir": "greets",   "lws": "JENINPYR-new-2.LWS",   "legacy": False},
    "chase":    {"authoring": True,  "dir": "chase",    "lws": "CHASE.LWS",            "legacy": True},
    "fountain": {"authoring": True,  "dir": "fountain", "lws": "FOUNTAIN - final.LWS", "legacy": True},
    # city: pinned 2026-07-11 — Authoring/city/CITY1.LWS + 20 objects regenerate
    # the shipping CITY.FLD byte-identically with lwsread_legacy (b1/b3/b6 are
    # FLD-recovered LWOs, see Authoring/city/README.md).
    "city":     {"authoring": True,  "dir": "city",     "lws": "CITY1.LWS",            "legacy": True},
    # crash: pinned 2026-07-11 (470d7f1) — the vintage END laptop scene;
    # Authoring/crash/CRASH.LWS regenerates the shipping CRASH.FLD
    # byte-identically (verified with BOTH converter variants — the scene has
    # no luminous materials, so the VLUM era doesn't matter; legacy matches
    # the other pre-b441da6 shipping FLDs). EVERY scene now has authoring
    # sources — the fldpatch fallback below only remains as a safety net.
    "crash":    {"authoring": True,  "dir": "crash",    "lws": "CRASH.LWS",            "legacy": True},
    "pbrtest":  {"authoring": True,  "dir": "pbrtest",  "lws": "PBRTEST.LWS",          "legacy": False},
}


def scene_authoring_dir(scene):
    return os.path.join(REPO, "Authoring", SCENES[scene]["dir"])


def scene_lwsread(scene):
    return LWSREAD_LEGACY if SCENES[scene].get("legacy") else LWSREAD

FLD_BACKUPS = os.path.join(RUNTIME, "SCENES", ".backups")


def scene_fld(scene):
    return os.path.join(RUNTIME, "SCENES", scene.upper() + ".FLD")


def scene_sidecar(scene):
    return os.path.join(RUNTIME, "SCENES", scene.upper() + ".MAT")


# ── Backend save log ────────────────────────────────────────────────────────
# The FE already prints a "what changed" receipt (shell.html buildSaveDiff /
# renderSaveReceipt, editor b66): the diff the BROWSER intended to save, built
# from its dirty collections and its own pristine baselines. This is the other
# half — what actually HIT DISK, emitted by the writer itself, on SAVE rather
# than on edit. It sees what the FE structurally cannot:
#   * a save POSTed by a script, a second tab, or a stale page;
#   * a surface name that resolved onto MORE .lwo files than the editor showed;
#   * an edit whose value was already on disk, so no bytes moved at all;
#   * the real file size deltas, the RVSF mask transition, the FLD md5.
#
# The line format deliberately mirrors the receipt so the two read as one
# stream: the same group headers ("3 surface(s)"), the same
# "<name> · <key>: <old> → <new>" items with the same number formatting
# (editorFmtV), and a "wrote:" footer where the receipt shows 'server wrote:'.
# Only the tag differs — FE console lines are '[editor]', these are
# '[editor-be]' — so grep separates the halves.
#
# Unconditional, matching the FE (whose receipt is not flag-gated either): a
# save is rare and user-triggered, and the save whose log was switched off is
# exactly the one you later wish you had a record of. Cost is a getsize per
# written file plus one md5 of the regenerated FLD.
BE_TAG = "[editor-be]"


def _fmtv(v):
    """Value formatter mirroring shell.html editorFmtV: None -> em dash,
    near-integers bare, everything else three decimals."""
    if v is None:
        return "—"
    if isinstance(v, bool):
        return "1" if v else "0"
    try:
        n = float(v)
    except (TypeError, ValueError):
        return str(v)
    if n != n or n in (float("inf"), float("-inf")):
        return str(v)
    return str(int(round(n))) if abs(n - round(n)) < 1e-6 else f"{n:.3f}"


def _fmt_uv(uv):
    """(proj, sx, sy, sz, axis) -> 'planar 2/2/2 axis Y' for the save log."""
    if not uv:
        return "—"
    proj, sx, sy, sz, axis = uv
    names = ["planar", "cylindrical", "spherical", "cubic"]
    pn = names[int(proj)] if 0 <= int(proj) < len(names) else f"proj{proj}"
    ax = {1: "X", 2: "Y", 4: "Z"}.get(int(axis) & 0x7, str(int(axis) & 0x7))
    return f"{pn} {_fmtv(sx)}/{_fmtv(sy)}/{_fmtv(sz)} axis {ax}"


def _kwval(line):
    """Second token of an LWS 'Keyword value' line, or None."""
    t = (line or "").split()
    return t[1] if len(t) > 1 else None


def _probe(fn, *a):
    """Read an OLD value for the log. A logging read must never be the thing
    that fails a save, so an absent or malformed chunk degrades to '—'."""
    try:
        return fn(*a)
    except Exception:
        return None


def _md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


class SaveLog:
    """Per-save accumulator, flushed when the save returns. Saves are
    serialized by save_lock, so ONE module-global instance is enough and no
    write site needs a new parameter threaded through it.

    Group titles ending in '(s)' get their item count prefixed at flush time,
    which is what reproduces the FE receipt's '3 surface(s)' headers while
    still letting two call sites (e.g. both LWS light patchers) contribute to
    one group."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.what = None
        self.scene = None
        self.groups = []        # [(title, [item, ...])], first-seen order
        self.writes = []        # "<relpath>  <a> → <b> B  (<delta>)  <extra>"
        self.notes = []
        self.t0 = 0.0

    def begin(self, scene, what):
        self.reset()
        self.scene, self.what, self.t0 = scene, what, time.time()

    def change(self, title, item):
        for t, items in self.groups:
            if t == title:
                items.append(item)
                return
        self.groups.append((title, [item]))

    def note(self, text):
        """Something the FE would report as saved that did NOT move bytes."""
        self.notes.append(text)

    def wrote(self, path, before, extra=""):
        """Record a file that just hit disk. `before` is its size prior to the
        write (0 = created). The AFTER size is read back FROM DISK, so the log
        reports what the filesystem has, not what the writer believed."""
        try:
            after = os.path.getsize(path)
        except OSError:
            after = 0
        self.writes.append(f"{os.path.relpath(path, REPO)}  {before} → {after} B  "
                           f"({after - before:+d})" + (f"  {extra}" if extra else ""))

    def flush(self, code=None):
        if self.what is None:                 # begin() never ran — nothing to say
            return
        n = sum(len(items) for _, items in self.groups)
        if n or self.writes or self.notes:
            print(f"{BE_TAG} {self.scene} {self.what} — what hit disk:")
            for title, items in self.groups:
                head = f"{len(items)} {title}" if title.endswith("(s)") else title
                print(f"{BE_TAG}   {head}")
                for it in items:
                    print(f"{BE_TAG}     {it}")
            if self.writes:
                print(f"{BE_TAG}   wrote:")
                for w in self.writes:
                    print(f"{BE_TAG}     {w}")
            for t in self.notes:
                print(f"{BE_TAG}   note: {t}")
        print(f"{BE_TAG} {self.scene} {self.what} — {n} change(s), "
              f"{len(self.writes)} file(s), {time.time() - self.t0:.2f}s, "
              + (f"HTTP {code}" if code is not None else "RAISED before returning"))
        sys.stdout.flush()
        self.reset()


SAVELOG = SaveLog()

ALLOWED_PROPS = {"baseR", "baseG", "baseB", "diffuse", "specular",
                 "glossiness", "luminosity", "transparency", "reflection",
                 # smoothAngle is a NATIVE LWO/FLD field (MaxSmoothingAngle +
                 # the Surf_Smoothing flag), so on FLD-patched scenes (city/
                 # crash) it round-trips through fldpatch like diffuse/specular.
                 # But the LWO patcher (authoring scenes: greets/chase/fountain)
                 # has no smoothing chunk, so there it is peeled to the sidecar
                 # instead — see split_surface_sidecar_keys / SMOOTH_SIDECAR.
                 "smoothAngle"}
# Engine-only per-material dials. On authoring scenes these ride the LWO 'RVSF'
# sub-chunk → FLD → Material (RVSF_SURF_KEYS below); the .MAT sidecar path is the
# dead non-authoring fallback (its reader is retired, sidecar-elim endgame).
# refractIor: per-surface glass-refraction IOR (0 = unset -> the global
# glass_refract_ior render knob).
# waterProcedural: tri-state procedural-water override on the scene's water
# surface (-1 off / 0 auto -> global --water_procedural / 1 on).
# envBakeRes: per-surface env-probe bake face resolution (pow2 64..1024;
# 0 = unset -> the global env_bake_res / legacy sizing chain).
# envDynamic: 0/1 authored flag (ENVDYN Workstream A1, docs/
# ENVDYN_DISPLACEMENT_PLAN.md) marking this material's env probe for the live
# dynamic-mesh reflection overlay. RVSF-only (mask bit 0x400) — routed exactly
# like the other RVSF dials via RVSF_SURF_KEYS/pop_rev_ext_props below.
# specMul: per-material specular RESPONSE multiplier (scales the final deferred
# specular incl. env-specular; RVSF bit 0x800 — the author-side dial for
# sources whose specular reads wrong, e.g. the Polyhaven sandstone).
# envBakeOfsX/Y/Z: env-probe capture-point OFFSET in world units. The three
# share ONE RVSF bit (0x1000) carrying one 12-byte X/Y/Z payload — deliberately
# not a bit per component like tintR/G/B, since it is a single semantic vector
# and three bits would nearly fill the u16. Routing needs nothing special: they
# are ordinary RVSF keys here, and lwopatch.set_rev_ext groups them on the way
# to disk (all-zero -> no bit, so an untouched surface stays byte-identical).
SURF_SIDECAR_KEYS = {"aoStrength", "parallaxScale", "normalFlip", "tintR", "tintG", "tintB", "refractive", "refractIor", "envRefl", "envBakeRes", "waterProcedural", "envDynamic", "specMul", "envBakeOfsX", "envBakeOfsY", "envBakeOfsZ"}
# Of those, the ones that migrate to the LWO RVSF sub-chunk (sidecar-elim §1a),
# for authoring scenes. = SURF_SIDECAR_KEYS minus normalFlip, which pairs with
# the normal-map assignment (§1e) and stays on the sidecar for now. lwopatch's
# RVSF_FIELDS is the on-disk field table this maps onto.
RVSF_SURF_KEYS = SURF_SIDECAR_KEYS - {"normalFlip"}
# key -> RVSF mask bit, for the save log (which bit a write actually lit).
RVSF_BIT = {k: bit for (k, bit, _) in lwopatch.RVSF_FIELDS}
# smoothAngle has no LWO surface chunk the lwopatch understands, so on AUTHORING
# scenes it can't take the native FLD path (lwopatch.set_prop would raise). It
# persists to the sidecar there instead: the engine honors a
# 'surface|smoothAngle|value' override at the init normal rebuild
# (MakeFacesIndependent) regardless of --surf_smoothing_authored — the override
# always wins. FLD-patched scenes keep the native fldpatch path.
SMOOTH_SIDECAR = {"smoothAngle"}


def split_surface_sidecar_keys(scene, surfaces, warnings):
    """Strip SURF_SIDECAR_KEYS out of the save payload's surface dicts and
    persist them as sidecar prop lines. Mutates `surfaces` (drops entries that
    become empty). Returns a summary list. (Authoring scenes peel only
    normalFlip here — the numeric dials go to the LWO RVSF; see do_save_main.)"""
    if not surfaces:
        return []
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    # Authoring scenes: RVSF_SURF_KEYS -> LWO RVSF (pop_rev_ext_props), maps ->
    # LWO RVSM (save_maps_setdirs), and smoothAngle -> native LWO SMAN
    # (ALLOWED_PROPS -> lwopatch.set_prop('smoothAngle'), sidecar-elim §1.5). So
    # the ONLY per-surface key still peeled to the sidecar is normalFlip (a
    # green-channel parity on the assigned normal map, §1e — no authored data
    # today; RVSM carries a normalFlip byte for when it's driven). The dead
    # non-authoring fallback still peels every SURF_SIDECAR_KEYS.
    if SCENES[scene].get("authoring"):
        keys = SURF_SIDECAR_KEYS - RVSF_SURF_KEYS - SMOOTH_SIDECAR   # = {normalFlip}
    else:
        keys = SURF_SIDECAR_KEYS
    saved = []
    for name in list(surfaces):
        props = surfaces[name]
        if not isinstance(props, dict):
            continue
        side = {k: props.pop(k) for k in list(props) if k in keys}
        # Names are real surfaces here: bake_splits already rewrote any split
        # '#k' key onto its baked real name before routing (sidecar-elim
        # endgame: the '#k' collapse is retired).
        base = name
        for k, v in side.items():
            entries[(base, k)] = f"{float(v):.6g}"
            saved.append({"surface": base, "key": k})
        if not props:
            del surfaces[name]
    if saved:
        write_sidecar(sidecar, entries)
    return saved
ALLOWED_ROLES = {"albedo", "normal", "height", "roughness", "ao", "metallic"}


# ── Instance-split BAKE (persist runtime splits into the LWO sources) ──────
# The editor's "split instances" is a LIVE mechanism (Editor_SplitInstances
# clusters faces at runtime; parts are named "<base>#k"). On Save, when the
# payload carries split parts, the split is BAKED into the authoring .lwo:
# lwopatch.split_surface reassigns each non-primary polygon cluster to a
# fresh REAL surface ('momy' -> 'momy' + 'momy2'), the FLD is regenerated,
# and every '#k' key in the same payload is REWRITTEN onto the real name
# (momy#2 props/maps -> momy2|...). After a reload the parts are ordinary
# authored surfaces — no runtime split needed, and the '#k' collapse below
# never fires for them.
#
# Live<->LWO part matching is GEOMETRIC: the shell ships each live part's
# world centroid (Editor_SplitInstances "centroids"); the LWO clusters are
# matched by nearest distance (raw LWO coords + the object's LWS keyframe-0
# position — the converter's SwapYZ is a no-op). Engine face order differs
# from LWO polygon order (init chunking reorders), so order-based matching
# would swap identical-size parts (the two-mummies tie). Without centroids
# (old shell / missing data) the bake falls back to the order-based mapping
# with a warning to verify visually.
#
# NOT baked (fall back to the legacy '#k'-collapse-with-warning):
#   - scenes without authoring sources (crash) — splits stay live-only
#   - LWS-instanced copies (2x LoadObject of one file): one spatial cluster
#     per file, nothing to reassign (known follow-up: duplicate the LWO and
#     repoint the second LoadObject)
#   - a surface spanning multiple .lwo files, chained re-splits ("momy#2#2"),
#     objects with authored rotation/scale when centroids are required


def lws_object_key0(scene, lwo_fname):
    """(#LoadObject occurrences of `lwo_fname`, key0 (pos, rot, scale)) from
    the scene's LWS — the object-space -> world offset for centroid matching.
    (0, None) when the file isn't referenced."""
    path = os.path.join(scene_authoring_dir(scene), SCENES[scene]["lws"])
    try:
        lines = open(path, encoding="latin-1").read().splitlines()
    except OSError:
        return 0, None
    count, key0 = 0, None
    want = lwo_fname.lower()
    for i, raw in enumerate(lines):
        ln = raw.strip()
        if not ln.lower().startswith("loadobject"):
            continue
        nm = os.path.basename(ln.split(None, 1)[1].strip()).lower() if " " in ln else ""
        if nm != want:
            continue
        count += 1
        for j in range(i + 1, len(lines)):
            s = lines[j].strip()
            if s.startswith("ObjectMotion"):
                try:            # channels line, keys line, then key0's 9 floats
                    vals = lines[j + 3].split()
                    key0 = ([float(v) for v in vals[0:3]],
                            [float(v) for v in vals[3:6]],
                            [float(v) for v in vals[6:9]])
                except (IndexError, ValueError):
                    pass
                break
            if s.lower().startswith(("loadobject", "addnullobject")):
                break
    return count, key0


def _match_split_parts(analysis, live_cents, key0, warnings, base):
    """cluster index -> part number, matching live '#k' centroids to LWO
    clusters by nearest distance. None -> caller uses the order-based map."""
    if not live_cents:
        return None
    pos, rot, scale = key0 if key0 else ([0, 0, 0], [0, 0, 0], [1, 1, 1])
    if any(abs(r) > 0.01 for r in rot) or any(abs(s - 1) > 0.01 for s in scale):
        warnings.append(f"split '{base}': object has authored rotation/scale — "
                        "using order-based part mapping, VERIFY the parts "
                        "landed on the right instances")
        return None
    parts = {}
    claimed = set()
    world = [tuple(c + p for c, p in zip(cl["centroid"], pos))
             for cl in analysis["clusters"]]
    for name in sorted(live_cents, key=lambda n: int(n.rsplit("#", 1)[1])):
        k = int(name.rsplit("#", 1)[1])
        lc = live_cents[name]
        best, bd = -1, None
        for ci, wc in enumerate(world):
            if ci in claimed:
                continue
            d = sum((a - b) ** 2 for a, b in zip(wc, lc)) ** 0.5
            if bd is None or d < bd:
                best, bd = ci, d
        if best < 0:
            warnings.append(f"split '{base}': more live parts than LWO clusters "
                            f"('{name}' unmatched) — using order-based mapping")
            return None
        if bd > max(2.0 * analysis["radius"], 1.0):
            warnings.append(f"split '{base}': live part '{name}' is {bd:.1f} "
                            "units from the nearest LWO cluster — using "
                            "order-based mapping, VERIFY visually")
            return None
        claimed.add(best)
        parts[best] = k
    # Unmatched clusters (live session had fewer parts) get the next numbers.
    nxt = max(parts.values(), default=1) + 1
    for ci in range(len(analysis["clusters"])):
        if ci not in parts:
            parts[ci] = nxt
            nxt += 1
    if 1 not in parts.values():
        warnings.append(f"split '{base}': no live '#1' centroid — using "
                        "order-based mapping")
        return None
    return parts


def bake_splits(scene, payload, warnings):
    """Bake every split the payload references into the authoring .lwo files
    and REWRITE the payload's '#k' surface/map keys onto the real baked
    names. Mutates payload['surfaces'] / payload['maps'] in place. Returns
    the bake summary list (empty = nothing baked). The bake is driven ENTIRELY
    by payload['splits'] (base name -> {clusters, centroids}); parts are matched
    GEOMETRICALLY (_match_split_parts), never by parsing a '#k' suffix off a
    name (sidecar-elim endgame: the '#k' collapse scaffolding is retired)."""
    surfaces = payload.get("surfaces") or {}
    maps = payload.get("maps") or {}
    splits = payload.get("splits") or {}
    bases = {}
    for b, info in splits.items():
        bases[b] = info if isinstance(info, dict) else {"clusters": info}
    if not bases:
        return []
    if not SCENES[scene].get("authoring"):
        warnings.append("scene has no authoring sources — instance splits stay "
                        "live-only ('#k' keys collapse onto the base surface)")
        return []
    adir = scene_authoring_dir(scene)
    lwos = {f: lwopatch.LwoFile(os.path.join(adir, f))
            for f in sorted(os.listdir(adir)) if f.lower().endswith(".lwo")}
    baked = []
    renames = {}
    for base, info in sorted(bases.items()):
        carriers = [f for f, lwo in lwos.items() if lwo.surface(base)]
        if not carriers:
            warnings.append(f"split '{base}': surface not in any .lwo — not baked")
            continue
        if len(carriers) > 1:
            warnings.append(f"split '{base}': surface spans {carriers} — bake "
                            "not supported, keys collapse onto the base")
            continue
        fname = carriers[0]
        nload, key0 = lws_object_key0(scene, fname)
        if nload > 1:
            warnings.append(f"split '{base}': {fname} is LWS-instanced x{nload} "
                            "— bake not supported (live-only split; known "
                            "follow-up: duplicate the LWO per instance)")
            continue
        lwo = lwos[fname]
        analysis = lwo.analyze_split(base)
        if analysis is None:
            warnings.append(f"split '{base}': one spatial cluster in {fname} — "
                            "nothing to bake")
            continue
        want = info.get("clusters")
        if want and int(want) != len(analysis["clusters"]):
            warnings.append(f"split '{base}': LWO clustering found "
                            f"{len(analysis['clusters'])} clusters, the live "
                            f"session had {want} — VERIFY the parts")
        mapping = _match_split_parts(analysis, info.get("centroids"), key0,
                                     warnings, base)
        if mapping is None:
            mapping = lwo.default_split_parts(analysis)
            if not info.get("centroids"):
                warnings.append(f"split '{base}': no live part centroids — "
                                "order-based part mapping, VERIFY the parts "
                                "landed on the right instances")
        res = lwo.commit_split(analysis, mapping)
        path = os.path.join(adir, fname)
        before = os.path.getsize(path)
        bak = lwopatch.backup(path, scene_backup_dir(scene))
        with open(path, "wb") as f:
            f.write(lwo.serialize())
        SAVELOG.change("split(s)", f"{base}: {len(analysis['clusters'])} part(s) → "
                       + "/".join(res["parts"][k] for k in sorted(res["parts"]))
                       + f" baked as real surfaces in {fname}")
        SAVELOG.wrote(path, before, f"backup {os.path.relpath(bak, REPO)}")
        baked.append({"file": fname, "surface": base, "parts": res["parts"],
                      "polys": res["polys"],
                      "backup": os.path.relpath(bak, REPO)})
        for k, real in res["parts"].items():
            renames[f"{base}#{k}"] = real
    # Rewrite the payload's '#k' keys onto the baked real names.
    for d in (surfaces, maps):
        for old in [k for k in d if k in renames]:
            new = renames[old]
            if new in d and isinstance(d[new], dict) and isinstance(d[old], dict):
                d[new].update(d.pop(old))
            else:
                d[new] = d.pop(old)
    return baked

# UV mapping pseudo-props: routed to lwopatch.set_uv_mapping (greets) /
# fldpatch.patch_material_uv (FLD scenes). The editor sends the FULL set
# whenever any of them changes.
UV_KEYS = {"uvProj", "uvScaleX", "uvScaleY", "uvScaleZ", "uvAxis"}


def pop_uv_props(surfaces, warnings):
    """Extract complete UV parameter sets out of the surface payload.
    Returns {name: (proj, sx, sy, sz, axis)}; partial sets warn + drop."""
    out = {}
    for name in list(surfaces):
        props = surfaces[name]
        if not isinstance(props, dict):
            continue
        uv = {k: props.pop(k) for k in list(props) if k in UV_KEYS}
        if not props:
            del surfaces[name]
        if not uv:
            continue
        if set(uv) != UV_KEYS:
            warnings.append(f"'{name}': partial UV set {sorted(uv)} — skipped "
                            "(editor sends all five together)")
            continue
        proj = int(uv["uvProj"])
        if proj < 0 or proj > 3:
            warnings.append(f"'{name}': uvProj {proj} out of range — skipped")
            continue
        out[name] = (proj, float(uv["uvScaleX"]),
                     float(uv["uvScaleY"]), float(uv["uvScaleZ"]),
                     int(uv["uvAxis"]))
    return out

def pop_rev_ext_props(surfaces):
    """Pull RVSF_SURF_KEYS (engine-only per-surface dials) out of the surface
    payload into {name: {key: value}} for the LWO RVSF sub-chunk writer
    (lwopatch.set_rev_ext). Removes emptied surface entries. Authoring scenes
    only — the dead non-authoring fallback already sidecar'd them. See
    docs/SIDECAR_MIGRATION_PLAN.md §1a."""
    out = {}
    for name in list(surfaces):
        props = surfaces[name]
        if not isinstance(props, dict):
            continue
        ext = {k: props.pop(k) for k in list(props) if k in RVSF_SURF_KEYS}
        if ext:
            out[name] = ext
        if not props:
            del surfaces[name]
    return out


# Live-served paths: the wasm preload (DEMO.data) copy of these is link-time
# stale, so the editor fetches them fresh from Runtime/ at boot. Prefix match.
# All of TEXTURES/ (not just TEXTURES/PBR/): the editor's "generate maps from
# albedo" feature fetches a surface's current diffuse texture (e.g.
# TEXTURES/greets_wall.png, TEXTURES/PELLOW.JPG) as the albedo source — those
# baked textures live in DEMO.data, not on the wasm-root, so serve them here.
LIVE_PREFIXES = ("/SCENES/", "/TEXTURES/")

save_lock = threading.Lock()


def ensure_lwsread():
    if os.path.exists(LWSREAD) and os.path.exists(LWSREAD_LEGACY):
        return
    src = os.path.join(REPO, "tools", "lwsread")
    print("[server] building tools/lwsread (+legacy) ...")
    subprocess.run(["cmake", "-S", src, "-B", os.path.join(src, "build")],
                   check=True, capture_output=True)
    subprocess.run(["cmake", "--build", os.path.join(src, "build")],
                   check=True, capture_output=True)


def map_surface_name(name):
    """editor surface name -> (lwo_filename or None, surf_name)."""
    name = re.sub(r"::mirUV$", "", name)
    # Names are real surfaces: bake_splits rewrote any split '#k' key onto its
    # baked real name before routing (sidecar-elim endgame — '#k' collapse
    # retired).
    if "::" in name:
        obj, surf = name.split("::", 1)
        surf = re.sub(r"_(body|upper)$", "", surf)
        return obj, surf
    return None, name


def read_sidecar(path):
    """Sidecar -> {(surface, key): value-string}. Holds both map lines
    (key=role, value=path) and numeric prop lines (key=prop, value=number)."""
    entries = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("|", 2)
            if len(parts) == 3:
                entries[(parts[0], parts[1])] = parts[2]
    return entries


def write_sidecar(path, entries):
    # The .MAT sidecar READER is retired (sidecar-elim endgame): the migrated
    # per-surface values ride the LWO RVSF/RVSM/SMAN sources, not this file.
    # Only the not-yet-FLD-backed leftovers still land here (obj:<name>|scale,
    # surface|normalFlip — SIDECAR_MIGRATION_PLAN §1d/§1e), and NOTHING loads
    # them at runtime today; a Save touching only those keys writes a file the
    # engine ignores. Kept so those write paths don't crash pending their FLD
    # records; the non-authoring fallback (dead — every scene is authored) also
    # still uses it.
    lines = ["# Editor leftovers — written by tools/editor_server.py (editor",
             "# \"Save\"). The .MAT reader is RETIRED (sidecar-elim endgame): the",
             "# engine does NOT load this file. Only not-yet-FLD-backed keys land",
             "# here (obj scale / normalFlip); they do not persist to the render.",
             "# Format: surface|prop|value  /  obj:<name>|scale|value", ""]
    for (surface, key), value in sorted(entries.items()):
        lines.append(f"{surface}|{key}|{value}")
    # Save log: diff what is on disk against what we are about to write. This
    # is the one choke point every sidecar writer funnels through (maps, object
    # scale, normalFlip, the dead non-authoring props), so instrumenting it
    # here covers them all without touching a single caller.
    was = read_sidecar(path)
    before = os.path.getsize(path) if os.path.exists(path) else 0
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    for k in sorted(set(was) | set(entries)):
        if was.get(k) != entries.get(k):
            SAVELOG.change("sidecar key(s)",
                           f"{k[0]} · {k[1]}: {_fmtv(was.get(k))} → {_fmtv(entries.get(k))}")
    SAVELOG.wrote(path, before, f"{len(entries)} key(s) — NOTE: the .MAT reader "
                                "is retired, the engine does not load this")


# PBR map SET layout (sidecar-elim §1e, USER DESIGN 2026-07-31): a texture set
# is a directory TEXTURES/PBR/<set>/ holding FIXED role filenames; a surface's
# LWO RVSM sub-chunk names ONE set, and the engine loads whichever role files
# exist. The editor default set = the surface's base name (dir-per-material);
# sets are shareable (two surfaces may name the same set). See
# MaterialImport_ApplyRevMaps.
def map_set_name(surface):
    """editor surface name -> texture-set dir name (base name, filesystem-safe).
    Names are real surfaces here (bake_splits rewrote split '#k' keys onto their
    baked real names before routing — the '#k' collapse is retired)."""
    base = re.sub(r"::mirUV$", "", surface)
    if "::" in base:
        base = base.split("::", 1)[1]
        base = re.sub(r"_(body|upper)$", "", base)
    return re.sub(r"[^A-Za-z0-9._-]+", "_", base)


def save_maps_setdirs(scene, maps, warnings):
    """Authoring-scene map save (§1e): write role files into
    Runtime/TEXTURES/PBR/<set>/<role>.png (set = surface base name) and return
    (written, rvsm) where rvsm = {surface: set-name or None-to-clear} to drive
    the LWO RVSM write-back. No sidecar map lines — the assignment lives in the
    LWO. A role reset (spec None) deletes that role file; when the set dir has
    no role files left, the surface's RVSM is cleared (set -> None)."""
    written, rvsm = [], {}
    for surface, roles in (maps or {}).items():
        if not isinstance(roles, dict):
            warnings.append(f"maps['{surface}']: bad shape — skipped")
            continue
        st = map_set_name(surface)
        setdir = os.path.join(PBR_DIR, st)
        for role, spec in roles.items():
            if role not in ALLOWED_ROLES:
                warnings.append(f"maps['{surface}']['{role}']: unknown role — skipped")
                continue
            dst = os.path.join(setdir, role + ".png")
            rel = f"TEXTURES/PBR/{st}/{role}.png"
            if spec is None:                       # editor "reset map"
                if os.path.exists(dst):
                    gone = os.path.getsize(dst)
                    os.remove(dst)
                    SAVELOG.change("map(s)", f"{surface} · {role}: {rel} → — "
                                             f"(deleted, {gone} B freed)")
                    written.append({"surface": surface, "set": st, "role": role, "deleted": True})
                continue
            data = base64.b64decode(spec.get("data") or "")
            if not data:
                warnings.append(f"maps['{surface}']['{role}']: empty data — skipped")
                continue
            os.makedirs(setdir, exist_ok=True)
            before = os.path.getsize(dst) if os.path.exists(dst) else 0
            with open(dst, "wb") as f:
                f.write(data)
            SAVELOG.change("map(s)", f"{surface} · {role}: "
                                     f"{'—' if not before else str(before) + ' B'} → {rel}")
            SAVELOG.wrote(dst, before)
            written.append({"surface": surface, "set": st, "role": role,
                            "path": f"TEXTURES/PBR/{st}/{role}.png", "bytes": len(data)})
        # RVSM: point the surface at its set, or clear it if the set dir is now
        # empty of role files (all roles reset).
        have = os.path.isdir(setdir) and any(
            os.path.exists(os.path.join(setdir, r + ".png")) for r in ALLOWED_ROLES)
        rvsm[surface] = st if have else None
    return written, rvsm


def save_maps(scene, maps, warnings):
    """{"<surface>": {"<role>": {"filename": ..., "data": base64}}} -> write the
    bytes under Runtime/TEXTURES/PBR/ and update the scene sidecar. Returns the
    list of written entries. Deterministic filenames so a re-import of the same
    slot overwrites instead of accumulating files. (Non-authoring fallback only;
    authoring scenes use save_maps_setdirs -> LWO RVSM.)"""
    written = []
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    for surface, roles in (maps or {}).items():
        if not isinstance(roles, dict):
            warnings.append(f"maps['{surface}']: bad shape — skipped")
            continue
        for role, spec in roles.items():
            if role not in ALLOWED_ROLES:
                warnings.append(f"maps['{surface}']['{role}']: unknown role — skipped")
                continue
            if spec is None:
                # Editor "reset map": delete the sidecar override so the next
                # load keeps the authored default. The PBR file (if any) stays
                # on disk — unreferenced, and a later re-import overwrites it.
                if (surface, role) in entries:
                    SAVELOG.change("map(s)", f"{surface} · {role}: "
                                             f"{entries[(surface, role)]} → — "
                                             "(sidecar line dropped; the PBR file stays)")
                    del entries[(surface, role)]
                    written.append({"surface": surface, "role": role, "deleted": True})
                continue
            fname = os.path.basename(spec.get("filename") or "")
            ext = os.path.splitext(fname)[1].lower() or ".png"
            data = base64.b64decode(spec.get("data") or "")
            if not data:
                warnings.append(f"maps['{surface}']['{role}']: empty data — skipped")
                continue
            # greets predates the scene prefix (momy_albedo.png &c) — keep its
            # naming stable; other scenes prefix to avoid cross-scene collisions.
            stem = f"{surface}_{role}" if scene == "greets" else f"{scene}_{surface}_{role}"
            safe = re.sub(r"[^A-Za-z0-9._-]+", "_", stem + ext)
            os.makedirs(PBR_DIR, exist_ok=True)
            dst = os.path.join(PBR_DIR, safe)
            before = os.path.getsize(dst) if os.path.exists(dst) else 0
            with open(dst, "wb") as f:
                f.write(data)
            rel = f"TEXTURES/PBR/{safe}"
            SAVELOG.change("map(s)", f"{surface} · {role}: "
                                     f"{_fmtv(entries.get((surface, role)))} → {rel}")
            SAVELOG.wrote(dst, before)
            entries[(surface, role)] = rel
            written.append({"surface": surface, "role": role, "path": rel,
                            "bytes": len(data), "original": fname})
    if written:
        write_sidecar(sidecar, entries)
    return written


def save_props_to_sidecar(scene, surfaces, warnings):
    """Numeric surface edits for scenes WITHOUT pinned LWO sources: persist as
    sidecar prop lines (applied at scene init). Returns per-surface summary."""
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    saved = []
    for name, props in surfaces.items():
        bad = set(props) - ALLOWED_PROPS
        if bad:
            warnings.append(f"'{name}': unknown props {sorted(bad)} — skipped")
            continue
        for p, v in props.items():
            entries[(name, p)] = f"{float(v):.6g}"
        saved.append({"surface": name, "props": sorted(props)})
    if saved:
        write_sidecar(sidecar, entries)
    return saved


# Per-OBJECT overrides — engine-only (TriMesh::EditorScale), persisted as
# sidecar "obj:<name>|scale|<v>" lines for EVERY scene type (authoring and
# FLD-patched alike; there is no LWO slot, and rewriting every LWS keyframe's
# scale channel was judged more invasive than the sidecar). <name> is the
# entry's 'obj' field from editorGetObjects (raw chunk-collapsed FLD object
# name). A scale of 1 DELETES the line (authored default).
OBJECT_KEYS = {"scale"}


def save_objects_to_sidecar(scene, objects, warnings):
    """{"<obj-name>": {"scale": v}} -> sidecar obj: lines. Returns summary."""
    if not objects or not isinstance(objects, dict):
        return []
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    saved = []
    changed = False
    for name, props in objects.items():
        if not isinstance(props, dict):
            warnings.append(f"objects['{name}']: bad shape — skipped")
            continue
        bad = set(props) - OBJECT_KEYS
        if bad:
            warnings.append(f"object '{name}': unknown keys {sorted(bad)} — skipped")
            continue
        for k, v in props.items():
            key = (f"obj:{name}", k)
            # Absent override == authored default 1, the same baseline the FE
            # receipt assumes (editorObjPristine[n] ?? 1).
            old = entries.get(key, "1" if k == "scale" else None)
            if k == "scale" and abs(float(v) - 1.0) < 1e-6:
                if key in entries:      # back to authored — drop the override
                    SAVELOG.change("object(s)", f"{name} · {k}: {_fmtv(old)} → 1 "
                                                "(override dropped)")
                    del entries[key]
                    changed = True
                    saved.append({"object": name, "key": k, "deleted": True})
                continue
            SAVELOG.change("object(s)", f"{name} · {k}: {_fmtv(old)} → {_fmtv(v)}")
            entries[key] = f"{float(v):.6g}"
            changed = True
            saved.append({"object": name, "key": k})
    if changed:
        write_sidecar(sidecar, entries)
        # The .MAT reader is retired (sidecar-elim endgame) and FdsObjectScale
        # (SIDECAR_MIGRATION_PLAN §1d) is not implemented, so this scale is NOT
        # loaded back at runtime yet — say so rather than fail silently.
        warnings.append("object scale saved to .MAT but NOT loaded at runtime "
                        "(reader retired; FdsObjectScale unimplemented, §1d)")
    return saved


LIGHT_KEYS = {"r", "g", "b", "intensity", "range"}
# Per-light engine extensions authored as LWS keywords INSIDE the light's
# AddLight block (the FdsFlareScale precedent — the whole FLD light-flag +
# conditional-payload + engine path already exists: Light_FlareScale bit 4096
# in tools/lwsread + FDS/FLD/FLD_READ.CPP → Omni::FlareScale). Migrated OFF the
# sidecar (sidecar-elimination, docs/SIDECAR_MIGRATION_PLAN.md §1c). key -> LWS
# keyword name. See pop_light_lws_keys / patch_lws_light_ext.
LIGHT_LWS_KEYS = {"flareScale": "FdsFlareScale"}
# Per key: the value that means "authored default / unset". Writing it DELETES
# the keyword instead of emitting it, so a light dialed back to default
# regenerates a byte-identical FLD (no flag bit, no payload). FlareScale 0/1
# both mean 1.0 in the engine (Omni::FlareScale sentinel); 1.0 is the editor's
# neutral, so treat it as unset.
LIGHT_LWS_IDENTITY = {"flareScale": 1.0}
# Non-authoring fallback (dead: every scene is source-authored now — see the
# plan §2) has no LWS to write, so it still peels engine-only light keys to the
# sidecar. Only reached when a scene has no authoring sources.
LIGHT_SIDECAR_KEYS = set(LIGHT_LWS_KEYS)


def note_unsaved(unsaved, warnings, idx, keys, why):
    """Record light keys that did NOT reach disk. Both halves matter: the
    warning is what the human reads, `unsaved` is what the FE consumes to keep
    those keys dirty instead of adopting them as the new pristine baseline."""
    if not keys:
        return
    warnings.append(f"light {idx}: {sorted(keys)} NOT persisted — {why}")
    if unsaved is not None:
        unsaved.append({"index": int(idx), "keys": sorted(keys), "why": why})


def split_light_sidecar_keys(scene, lights, warnings):
    """Strip engine-only light keys to sidecar light: lines — ONLY for the dead
    non-authoring fallback. Authoring scenes route them to LWS keywords instead
    (pop_light_lws_keys / patch_lws_light_ext), so nothing is stripped there.
    Mutates `lights` (drops entries that become empty). Returns a summary list."""
    if not lights:
        return []
    # Authoring scenes: keep the keys in the payload for the LWS-keyword path.
    keys = set() if SCENES[scene].get("authoring") else LIGHT_SIDECAR_KEYS
    if not keys:
        return []
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    saved = []
    for idx_s in list(lights):
        props = lights[idx_s]
        if not isinstance(props, dict):
            continue
        side = {k: props.pop(k) for k in list(props) if k in keys}
        for k, v in side.items():
            entries[(f"light:{int(idx_s)}", k)] = f"{float(v):.6g}"
            saved.append({"light": int(idx_s), "key": k})
        if not props:
            del lights[idx_s]
    if saved:
        write_sidecar(sidecar, entries)
    return saved


def pop_light_lws_keys(lights):
    """Pull LWS-keyword light props (flareScale) out of the light dicts so
    patch_lws_lights (native AddLight-block fields only) doesn't reject them as
    unknown. Mutates `lights` (drops entries that become empty). Returns
    {idx_str: {key: value}} for patch_lws_light_ext."""
    out = {}
    for idx_s in list(lights):
        props = lights[idx_s]
        if not isinstance(props, dict):
            continue
        ext = {k: props.pop(k) for k in list(props) if k in LIGHT_LWS_KEYS}
        if ext:
            out[idx_s] = ext
        if not props:
            del lights[idx_s]
    return out


def patch_lws_light_ext(scene, light_ext, warnings):
    """{idx_str: {flareScale: v}} -> per-light LWS keyword lines inside the
    light's AddLight block (e.g. 'FdsFlareScale 0.15'). A value equal to the
    key's LIGHT_LWS_IDENTITY DELETES the keyword (keeps the regenerated FLD
    byte-identical to an unauthored light). Returns [{index, keys, file,
    backup}]; writes + backs up the LWS only if something changed.

    Processes light blocks HIGHEST index first so an insert (a light that had
    no such keyword yet) never shifts a lower block's precomputed span."""
    if not light_ext:
        return []
    lws_name = SCENES[scene]["lws"]
    path = os.path.join(scene_authoring_dir(scene), lws_name)
    orig = open(path, encoding="latin-1").read()
    lines = orig.split("\n")
    starts = [i for i, l in enumerate(lines) if l.strip() == "AddLight"]
    patched = []
    for idx_s in sorted(light_ext, key=lambda s: int(s), reverse=True):
        idx = int(idx_s)
        if idx < 0 or idx >= len(starts):
            warnings.append(f"light {idx}: LWS has {len(starts)} AddLight blocks — skipped")
            continue
        lo = starts[idx]
        hi = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
        done = []
        for key, val in light_ext[idx_s].items():
            kw = LIGHT_LWS_KEYS[key]
            try:
                fv = float(val)
            except (TypeError, ValueError):
                warnings.append(f"light {idx}.{key}: bad value {val!r} — skipped")
                continue
            fi = next((i for i in range(lo, hi)
                       if lines[i].split(None, 1)[:1] == [kw]), None)
            if abs(fv - LIGHT_LWS_IDENTITY[key]) < 1e-6:   # back to default → drop
                if fi is not None:
                    SAVELOG.change("light(s)", f"light {idx} · {key}: "
                                   f"{_fmtv(_kwval(lines[fi]))} → — ({kw} dropped)")
                    del lines[fi]
                    done.append(key)
                continue
            newline = f"{kw} {fv:.6g}"
            if fi is not None:
                if lines[fi] != newline:
                    SAVELOG.change("light(s)", f"light {idx} · {key}: "
                                   f"{_fmtv(_kwval(lines[fi]))} → {_fmtv(fv)}")
                    lines[fi] = newline
                    done.append(key)
            else:
                # Insert after LgtIntensity (present in every AddLight block),
                # else right after the AddLight line — the parser only needs the
                # keyword somewhere inside the light section (sets CurLight).
                at = next((i for i in range(lo, hi)
                           if lines[i].split(None, 1)[:1] == ["LgtIntensity"]), lo)
                SAVELOG.change("light(s)", f"light {idx} · {key}: — → {_fmtv(fv)} "
                                           f"({kw} inserted)")
                lines.insert(at + 1, newline)
                done.append(key)
        if done:
            patched.append({"index": idx, "keys": sorted(done)})
    if patched:
        new = "\n".join(lines)
        if new != orig:
            before = os.path.getsize(path)
            bak = lwopatch.backup(path, scene_backup_dir(scene))
            with open(path, "w", encoding="latin-1") as f:
                f.write(new)
            SAVELOG.wrote(path, before, f"backup {os.path.relpath(bak, REPO)}")
            for p in patched:
                p["file"] = lws_name
                p["backup"] = os.path.relpath(bak, REPO)
        else:
            patched = []
            SAVELOG.note(f"{lws_name}: light keywords already at those values "
                         "— not written")
    return patched


def patch_lws_lights(scene, lights, warnings, unsaved=None):
    """{"<index>": {r,g,b,intensity,range}} -> patch the i-th AddLight block of
    the scene's LWS (LWSC v1 is line-based text: LightColor R G B /
    LgtIntensity F / LightRange F). Index order == the engine's
    Omni_SceneAuthored order == AddLight file order. Returns list of patched
    entries; writes + backs up the LWS only if something changed.

    `unsaved` (optional list) collects {index, keys, why} for every key this
    writer did NOT put on disk — see note_unsaved. The caller ships it back to
    the browser so the FE can keep those keys dirty."""
    if not lights:
        return []
    lws_name = SCENES[scene]["lws"]
    path = os.path.join(scene_authoring_dir(scene), lws_name)
    lines = open(path, encoding="latin-1").read().split("\n")
    # Block spans: AddLight line -> next AddLight (or EOF). Camera section
    # follows the last light; patching only known keys inside a span is safe.
    starts = [i for i, l in enumerate(lines) if l.strip() == "AddLight"]
    patched = []
    for idx_s, props in sorted(lights.items(), key=lambda kv: int(kv[0])):
        idx = int(idx_s)
        if idx < 0 or idx >= len(starts):
            warnings.append(f"light {idx}: LWS has {len(starts)} AddLight blocks — skipped")
            note_unsaved(unsaved, warnings, idx, sorted(props),
                         f"LWS has only {len(starts)} AddLight blocks")
            continue
        # Skip granularity: an unknown key drops ITSELF, not the whole light.
        # Until 2026-08 a `continue` here threw the entire light away — save
        # {intensity, coneAngle} and the intensity edit vanished too, while the
        # response still said ok:true and the FE cleared its dirty flag. Every
        # key this writer knows is still written; the rest is reported.
        bad = sorted(set(props) - LIGHT_KEYS)
        if bad:
            note_unsaved(unsaved, warnings, idx, bad,
                         "not a native AddLight scalar field this writer knows")
            props = {k: v for k, v in props.items() if k in LIGHT_KEYS}
            if not props:
                continue
        lo = starts[idx]
        hi = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
        done = set()
        for i in range(lo, hi):
            stripped = lines[i].strip()
            if stripped.startswith("LightColor ") and {"r", "g", "b"} & set(props):
                old = stripped.split()
                r = int(round(float(props.get("r", old[1]))))
                g = int(round(float(props.get("g", old[2]))))
                b = int(round(float(props.get("b", old[3]))))
                for ci, c in ((1, "r"), (2, "g"), (3, "b")):
                    if c in props:
                        SAVELOG.change("light(s)", f"light {idx} · {c}: "
                                       f"{_fmtv(old[ci])} → {_fmtv(props[c])}")
                lines[i] = f"LightColor {r} {g} {b}"
                done |= {"r", "g", "b"} & set(props)
            elif stripped.startswith("LgtIntensity ") and "intensity" in props:
                SAVELOG.change("light(s)", f"light {idx} · intensity: "
                               f"{_fmtv(_kwval(stripped))} → {_fmtv(props['intensity'])}")
                lines[i] = f"LgtIntensity {float(props['intensity']):.6f}"
                done.add("intensity")
            elif stripped.startswith("LightRange ") and "range" in props:
                SAVELOG.change("light(s)", f"light {idx} · range: "
                               f"{_fmtv(_kwval(stripped))} → {_fmtv(props['range'])}")
                lines[i] = f"LightRange {float(props['range']):.6f}"
                done.add("range")
        missing = sorted(set(props) - done)
        if missing:
            note_unsaved(unsaved, warnings, idx, missing,
                         "no such line in its AddLight block (envelope-animated?)")
        if done:
            patched.append({"index": idx, "keys": sorted(done)})
    if patched:
        new = "\n".join(lines)
        if new != open(path, encoding="latin-1").read():
            before = os.path.getsize(path)
            bak = lwopatch.backup(path, scene_backup_dir(scene))
            with open(path, "w", encoding="latin-1") as f:
                f.write(new)
            SAVELOG.wrote(path, before, f"backup {os.path.relpath(bak, REPO)}")
            for p in patched:
                p["file"] = lws_name
                p["backup"] = os.path.relpath(bak, REPO)
        else:
            patched = []   # values identical — nothing actually changed
            SAVELOG.note(f"{lws_name}: light values identical — not written")
    return patched


# Scene-wide env-reflection defaults: authored as TOP-LEVEL LWS KEYWORDS
# (the VolumetricLight precedent), parsed by tools/lwsread into a flag-gated
# conditional payload in the FLD scene header (Scene_EnvDefaults bit on the
# AmbientIntensity envelope's EndBehavior), read at FLD load into
# Scene::EnvReflSceneMode/EnvBakeResScene, consumed by
# EnvReflection_FramePrep. NOT sidecar keys — sidecars are being eliminated;
# persistence belongs in the LWO/LWS sources.
SCENE_ENV_LWS_KEYS = {"envRefl": "FdsSceneEnvRefl",
                      "envBakeRes": "FdsSceneEnvBakeRes"}


def patch_lws_scene_env(scene, scene_env, warnings):
    """{'envRefl': -1|0|1, 'envBakeRes': N} -> top-level LWS keyword lines
    (value 0 DELETES the line = back to unauthored). Returns the list of
    patched keys ([] = nothing changed); writes the LWS with a backup."""
    if not scene_env or not isinstance(scene_env, dict):
        return []
    bad = set(scene_env) - set(SCENE_ENV_LWS_KEYS)
    if bad:
        warnings.append(f"sceneEnv: unknown keys {sorted(bad)} — skipped")
    path = os.path.join(scene_authoring_dir(scene), SCENES[scene]["lws"])
    text = open(path, encoding="latin-1").read()
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()
    patched = []
    for key in sorted(set(scene_env) & set(SCENE_ENV_LWS_KEYS)):
        kw = SCENE_ENV_LWS_KEYS[key]
        try:
            val = int(float(scene_env[key]))
        except (TypeError, ValueError):
            warnings.append(f"sceneEnv.{key}: bad value {scene_env[key]!r} — skipped")
            continue
        if key == "envRefl" and val not in (-1, 0, 1):
            warnings.append(f"sceneEnv.envRefl: {val} not in -1/0/1 — skipped")
            continue
        if key == "envBakeRes" and val != 0 and val not in (64, 128, 256, 512, 1024):
            warnings.append(f"sceneEnv.envBakeRes: {val} not a pow2 in 64..1024 — skipped")
            continue
        idx = next((i for i, ln in enumerate(lines)
                    if ln.split(None, 1)[:1] == [kw]), None)
        if val == 0:
            if idx is not None:
                SAVELOG.change("scene env defaults",
                               f"{key}: {_fmtv(_kwval(lines[idx]))} → — ({kw} dropped)")
                del lines[idx]
                patched.append(key)
            continue
        newline = f"{kw} {val}"
        if idx is not None:
            if lines[idx] != newline:
                SAVELOG.change("scene env defaults",
                               f"{key}: {_fmtv(_kwval(lines[idx]))} → {_fmtv(val)}")
                lines[idx] = newline
                patched.append(key)
        else:
            # Insert after the FramesPerSecond line (every scene LWS has one)
            # so the keyword sits in the top-level scene section.
            at = next((i for i, ln in enumerate(lines)
                       if ln.split(None, 1)[:1] == ["FramesPerSecond"]),
                      len(lines) - 1) + 1
            SAVELOG.change("scene env defaults",
                           f"{key}: — → {_fmtv(val)} ({kw} inserted)")
            lines.insert(at, newline)
            patched.append(key)
    if patched:
        before = os.path.getsize(path)
        bak = lwopatch.backup(path, scene_backup_dir(scene))
        with open(path, "w", encoding="latin-1", newline="") as f:
            f.write(nl.join(lines) + nl)
        SAVELOG.wrote(path, before, f"backup {os.path.relpath(bak, REPO)}")
        warnings.append(f"scene env defaults -> {os.path.basename(path)} "
                        f"({', '.join(patched)}; backup "
                        f"{os.path.relpath(bak, REPO)})")
    return patched


def regen_fld(scene, patched):
    """Rerun the scene's converter (legacy for pre-b441da6 shipping FLDs —
    the VLUM x100 era) and install the FLD. Returns (code, response-dict)."""
    ensure_lwsread()
    adir = scene_authoring_dir(scene)
    tmp_fld = os.path.join(adir, ".regen.tmp.fld")
    r = subprocess.run([scene_lwsread(scene), SCENES[scene]["lws"], ".regen.tmp.fld"],
                       cwd=adir, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(tmp_fld):
        return 500, {"ok": False, "error": "lwsread failed",
                     "stderr": (r.stderr or r.stdout)[-2000:], "patched": patched}
    fld_install = scene_fld(scene)
    before = os.path.getsize(fld_install) if os.path.exists(fld_install) else 0
    shutil.move(tmp_fld, fld_install)
    # The md5 is the log's most useful field: this project gates regressions on
    # golden FLD hashes, so a save that should have been inert is caught here
    # rather than three steps later.
    nlwo = sum(1 for f in os.listdir(adir) if f.lower().endswith(".lwo"))
    SAVELOG.wrote(fld_install, before,
                  f"md5 {_md5(fld_install)}  ← {SCENES[scene]['lws']} + {nlwo} .lwo "
                  f"({os.path.basename(scene_lwsread(scene))})")
    return 200, {"ok": True, "patched": patched,
                 "fld": os.path.relpath(fld_install, REPO),
                 "fld_bytes": os.path.getsize(fld_install)}


def do_save_fld(scene, surfaces, lights, uv_by_name, saved_maps, warnings):
    """Write-back for scenes without pinned sources: patch the shipping FLD
    in place (tools/fldpatch.py — walk-validated), with a timestamped backup.
    Also drop any sidecar prop lines this save supersedes: sidecar overrides
    apply AFTER the FLD loads, so a stale line would shadow the patched value."""
    fld_path = scene_fld(scene)
    if not os.path.exists(fld_path):
        return 404, {"ok": False, "error": f"no FLD for scene '{scene}'"}
    fld = fldpatch.FldFile(fld_path)

    patched_surfaces, patched_lights = [], []
    for name, props in surfaces.items():
        bad = set(props) - ALLOWED_PROPS
        if bad:
            return 400, {"ok": False, "error": f"'{name}': unknown props {sorted(bad)}"}
        # Names are real surfaces: bake_splits rewrote any split '#k' key onto
        # its baked real name before routing (sidecar-elim endgame).
        n = fld.patch_material(name, props)
        if n == 0:
            warnings.append(f"'{name}': no material record in {os.path.basename(fld_path)} — skipped")
        else:
            patched_surfaces.append({"surface": name, "records": n, "props": sorted(props)})

    for name, uv in (uv_by_name or {}).items():
        n = fld.patch_material_uv(name, *uv)
        if n == 0:
            warnings.append(f"'{name}': UV target not in {os.path.basename(fld_path)} — skipped")
        else:
            patched_surfaces.append({"surface": name, "records": n, "props": ["uv-mapping"]})

    for idx_s, props in sorted(lights.items(), key=lambda kv: int(kv[0])):
        bad = set(props) - LIGHT_KEYS
        if bad:
            return 400, {"ok": False, "error": f"light {idx_s}: unknown keys {sorted(bad)}"}
        try:
            fld.patch_light(int(idx_s), props)
            patched_lights.append({"index": int(idx_s), "keys": sorted(props)})
        except ValueError as e:
            warnings.append(f"light {idx_s}: {e}")

    for p in patched_surfaces:
        SAVELOG.change("surface(s)", f"{os.path.basename(fld_path)}:{p['surface']} · "
                                     f"{', '.join(p['props'])} → patched in "
                                     f"{p['records']} record(s)")
    for p in patched_lights:
        SAVELOG.change("light(s)", f"light {p['index']} · {', '.join(p['keys'])} "
                                   f"→ patched in {os.path.basename(fld_path)}")
    wrote = None
    if patched_surfaces or patched_lights:
        if bytes(fld.data) != open(fld_path, "rb").read():
            before = os.path.getsize(fld_path)
            bak = lwopatch.backup(fld_path, FLD_BACKUPS)
            fld.save(fld_path)
            SAVELOG.wrote(fld_path, before, f"md5 {_md5(fld_path)}  "
                                            f"backup {os.path.relpath(bak, REPO)}")
            wrote = {"file": os.path.basename(fld_path),
                     "surfaces": [p["surface"] for p in patched_surfaces],
                     "backup": os.path.relpath(bak, REPO)}
        else:
            warnings.append("values identical to the FLD — nothing written")
            SAVELOG.note(f"{os.path.basename(fld_path)}: values identical — not written")

    # Migrate superseded sidecar prop lines out (map lines stay).
    if patched_surfaces:
        sidecar = scene_sidecar(scene)
        entries = read_sidecar(sidecar)
        removed = [k for k in entries
                   if any(k[0] == p["surface"] and k[1] in p["props"]
                          for p in patched_surfaces)]
        if removed:
            for k in removed:
                del entries[k]
            write_sidecar(sidecar, entries)
            warnings.append(f"migrated {len(removed)} sidecar prop line(s) into the FLD")

    return 200, {"ok": True,
                 "patched": [wrote] if wrote else [],
                 "props": patched_surfaces,
                 "lights": patched_lights,
                 "fld": os.path.relpath(fld_path, REPO) if wrote else None,
                 "maps": saved_maps,
                 "sidecar": os.path.relpath(scene_sidecar(scene), REPO) if saved_maps else None,
                 "warnings": warnings}


def do_save(scene, payload):
    """Bracket the save with the backend write log (SaveLog), then delegate.
    The try/finally matters: a save that raises part-way has still written
    whatever it wrote, and that is exactly the case the log exists for."""
    SAVELOG.begin(scene, "save")
    code = None
    try:
        code, resp = _do_save(scene, payload)
        return code, resp
    finally:
        SAVELOG.flush(code)


def _do_save(scene, payload):
    """Apply {"surfaces": {...}, "maps": {...}, "lights": {...},
    "objects": {...}, "splits": {...}}. Object props (scale) go to the
    sidecar for every scene type; splits are BAKED into the .lwo sources
    (bake_splits — payload '#k' keys are rewritten onto the real baked
    names before routing); the rest routes per scene — see do_save_main."""
    if scene not in SCENES:
        return 404, {"ok": False, "error": f"unknown scene '{scene}'"}
    obj_warnings = []
    saved_objects = save_objects_to_sidecar(scene, payload.get("objects") or {},
                                            obj_warnings)
    split_warnings = []
    split_baked = bake_splits(scene, payload, split_warnings)
    rest = dict(payload)
    rest.pop("objects", None)
    rest.pop("splits", None)
    code, resp = do_save_main(scene, rest)
    if split_baked:
        # A splits-only save legitimately has nothing else in the payload.
        if not resp.get("ok") and "no surfaces" in str(resp.get("error", "")):
            code, resp = 200, {"ok": True, "patched": [], "warnings": []}
        if code == 200:
            resp["split_baked"] = split_baked
            resp["patched"] = (resp.get("patched") or []) + [
                {"file": b["file"], "surfaces": sorted(b["parts"].values()),
                 "backup": b["backup"]} for b in split_baked]
            # The split changed the .lwo — the shipping FLD must pick it up
            # even when nothing else in the payload triggered a regen.
            if not resp.get("fld"):
                rcode, rresp = regen_fld(scene, [])
                if rcode != 200:
                    rresp.setdefault("warnings", []).extend(split_warnings)
                    return rcode, rresp
                resp["fld"] = rresp["fld"]
                resp["fld_bytes"] = rresp.get("fld_bytes")
    if saved_objects:
        # An objects-only save legitimately has nothing else in the payload.
        if not resp.get("ok") and "no surfaces" in str(resp.get("error", "")):
            code, resp = 200, {"ok": True, "patched": [], "warnings": []}
        resp["objects"] = saved_objects
        resp["sidecar"] = os.path.relpath(scene_sidecar(scene), REPO)
    if obj_warnings or split_warnings:
        resp.setdefault("warnings", []).extend(split_warnings + obj_warnings)
    return code, resp


def do_save_main(scene, payload):
    """Apply {"surfaces": {...}, "maps": {...}, "lights": {...},
    "sceneEnv": {...}}. Authoring scenes (all of them now): patch LWOs + LWS,
    regen + install the FLD; sceneEnv lands as top-level LWS keywords.
    Non-authoring fallback: surfaces persist as sidecar prop lines, lights
    are live-only (warned). Maps go to the scene sidecar either way."""
    surfaces = payload.get("surfaces") or {}
    maps = payload.get("maps") or {}
    lights = payload.get("lights") or {}
    scene_env = payload.get("sceneEnv") or {}
    if not isinstance(surfaces, dict) or not isinstance(lights, dict) \
       or (not surfaces and not maps and not lights and not scene_env):
        return 400, {"ok": False, "error": "no surfaces, maps, or lights in payload"}

    warnings = []
    # PBR maps: authoring scenes → set dirs + LWO RVSM (§1e); the dead
    # non-authoring fallback keeps the sidecar. map_rvsm = {surface: set|None}.
    map_rvsm = {}
    if SCENES[scene].get("authoring"):
        saved_maps, map_rvsm = save_maps_setdirs(scene, maps, warnings)
    else:
        saved_maps = save_maps(scene, maps, warnings)
    # Engine-only keys (light flareScale, surface aoStrength/parallaxScale) →
    # sidecar, for every scene type; what remains goes to the LWS/LWO/FLD
    # patchers below.
    saved_light_side = split_light_sidecar_keys(scene, lights, warnings)
    if saved_light_side:
        warnings.append(f"{len(saved_light_side)} light key(s) → .MAT (non-authoring fallback; the .MAT reader is RETIRED — not loaded)")
    saved_surf_side = split_surface_sidecar_keys(scene, surfaces, warnings)
    if saved_surf_side:
        warnings.append(f"{len(saved_surf_side)} surface key(s) (normalFlip) → .MAT — NOT loaded (reader retired; normalFlip's RVSM write-back is unimplemented, SIDECAR_MIGRATION_PLAN §1e)")
    uv_by_name = pop_uv_props(surfaces, warnings)
    # Every light key the writers below decline to put on disk lands here and
    # goes back to the browser as `lightsUnsaved`.
    light_unsaved = []

    if not SCENES[scene]["authoring"]:
        if scene_env:
            warnings.append("sceneEnv: scene has no authoring sources — scene "
                            "env defaults need LWS write-back, skipped")
        code, resp = do_save_fld(scene, surfaces, lights, uv_by_name, saved_maps, warnings)
        if isinstance(resp, dict):
            resp["lightsUnsaved"] = light_unsaved
        return code, resp

    # Engine-only per-surface keys (aoStrength/parallaxScale/tint/refractIor/
    # refractive/envRefl/envBakeRes/waterProcedural) → LWO RVSF sub-chunk,
    # popped BEFORE the ALLOWED_PROPS resolution (sidecar-elim §1a).
    rev_ext_by_name = pop_rev_ext_props(surfaces)
    # Engine-only per-light keys (flareScale) → LWS keywords in the light block,
    # popped BEFORE patch_lws_lights so it doesn't reject them (sidecar-elim §1c).
    light_ext = pop_light_lws_keys(lights)
    patched_scene_env = patch_lws_scene_env(scene, scene_env, warnings)
    patched_lights = patch_lws_lights(scene, lights, warnings, light_unsaved)
    patched_lights += patch_lws_light_ext(scene, light_ext, warnings)
    if not surfaces and not uv_by_name and not rev_ext_by_name and not map_rvsm:
        if patched_lights or patched_scene_env:
            # Lights / scene env defaults changed -> the FLD must be
            # regenerated (it embeds them).
            code, resp = regen_fld(scene, [])
            if code != 200:
                return code, resp
            resp.update({"maps": saved_maps, "lights": patched_lights,
                         "lightsUnsaved": light_unsaved,
                         "sceneEnv": patched_scene_env, "warnings": warnings})
            return 200, resp
        # Nothing for the LWO/FLD (non-authoring maps went to the sidecar, or
        # the payload had nothing map/surface-shaped): no FLD regen.
        return 200, {"ok": True, "patched": [], "maps": saved_maps,
                     "lightsUnsaved": light_unsaved,
                     "sidecar": os.path.relpath(scene_sidecar(scene), REPO) if saved_maps else None,
                     "warnings": warnings}

    adir = scene_authoring_dir(scene)
    lwos = {f: lwopatch.LwoFile(os.path.join(adir, f))
            for f in sorted(os.listdir(adir)) if f.lower().endswith(".lwo")}

    # (file, surf) -> {prop: value}; resolve editor names against actual files.
    per_file = {}
    for name, props in surfaces.items():
        if not isinstance(props, dict):
            return 400, {"ok": False, "error": f"bad props for '{name}'"}
        bad = set(props) - ALLOWED_PROPS
        if bad:
            return 400, {"ok": False, "error": f"'{name}': unknown props {sorted(bad)}"}
        fname, surf = map_surface_name(name)
        targets = []
        if fname is not None:
            lwo = lwos.get(fname)
            if lwo is None or lwo.surface(surf) is None:
                warnings.append(f"'{name}': no surface '{surf}' in {fname} — skipped")
                continue
            targets = [(fname, surf)]
        else:
            targets = [(f, surf) for f, lwo in lwos.items() if lwo.surface(surf)]
            if not targets:
                warnings.append(f"'{name}': surface not found in any .lwo — skipped")
                continue
        for key in targets:
            slot = per_file.setdefault(key, {})
            for p, v in props.items():
                if p in slot and slot[p] != v:
                    warnings.append(f"{key[0]}:{key[1]}.{p}: conflicting values "
                                    f"({slot[p]} vs {v}) — using {v}")
                slot[p] = v

    # RVSF (engine-only per-surface) props: same editor-name → .lwo resolution
    # as the numeric props above, applied via lwopatch.set_rev_ext.
    per_file_ext = {}   # (fname, surf) -> {rvsf_key: value}
    for name, props in rev_ext_by_name.items():
        fname, surf = map_surface_name(name)
        if fname is not None:
            lwo = lwos.get(fname)
            targets = [(fname, surf)] if (lwo and lwo.surface(surf) is not None) else []
            if not targets:
                warnings.append(f"'{name}': no surface '{surf}' in {fname} (RVSF) — skipped")
                continue
        else:
            targets = [(f, surf) for f, lwo in lwos.items() if lwo.surface(surf)]
            if not targets:
                warnings.append(f"'{name}': RVSF surface not found in any .lwo — skipped")
                continue
        for key in targets:
            slot = per_file_ext.setdefault(key, {})
            for p, v in props.items():
                if p in slot and slot[p] != v:
                    warnings.append(f"{key[0]}:{key[1]}.{p}: conflicting RVSF values "
                                    f"({slot[p]} vs {v}) — using {v}")
                slot[p] = v

    # RVSM (PBR map SET) assignments (§1e): resolve editor surface -> .lwo the
    # same way, apply via lwopatch.set_rev_maps. set-name -> {'set': name};
    # None (all roles reset -> empty set dir) -> {'set': None} clears the RVSM.
    per_file_rvsm = {}   # (fname, surf) -> set-name or None
    for name, st in map_rvsm.items():
        fname, surf = map_surface_name(name)
        if fname is not None:
            targets = ([(fname, surf)] if (lwos.get(fname)
                       and lwos[fname].surface(surf) is not None) else [])
            if not targets:
                warnings.append(f"'{name}': no surface '{surf}' in {fname} (RVSM) — skipped")
                continue
        else:
            targets = [(f, surf) for f, lwo in lwos.items() if lwo.surface(surf)]
            if not targets:
                warnings.append(f"'{name}': RVSM surface not found in any .lwo — skipped")
                continue
        for key in targets:
            per_file_rvsm[key] = st

    # UV mapping edits: resolve names the same way, patch CTEX/TSIZ/TFLG.
    uv_targets = {}   # (fname, surf) -> uv tuple
    for name, uv in uv_by_name.items():
        fname, surf = map_surface_name(name)
        targets = ([(fname, surf)] if fname is not None and fname in lwos
                                      and lwos[fname].surface(surf) is not None
                   else [(f, surf) for f, lwo in lwos.items() if lwo.surface(surf)])
        if not targets:
            warnings.append(f"'{name}': UV target not found in any .lwo — skipped")
            continue
        for key in targets:
            uv_targets[key] = uv
    # Save log group for everything surface-shaped below. Counted AFTER name
    # resolution — one entry per (.lwo, surface) the editor's names actually
    # landed on, which is the number the FE cannot know (a bare FLD name can
    # resolve into several .lwo files).
    G_SURF = "surface(s)"
    for (fname, surf), uv in sorted(uv_targets.items()):
        s = lwos[fname].surface(surf)
        SAVELOG.change(G_SURF, f"{fname}:{surf} · uv: "
                               f"{_fmt_uv(_probe(s.uv_mapping))} → {_fmt_uv(uv)}")
        s.set_uv_mapping(*uv)

    if not per_file and not uv_targets and not per_file_ext and not per_file_rvsm:
        if saved_maps or patched_lights or patched_scene_env:
            # surface edits all missed, but these landed
            code, resp = (regen_fld(scene, []) if (patched_lights or patched_scene_env)
                          else (200, {"ok": True, "patched": []}))
            if code != 200:
                return code, resp
            resp.update({"maps": saved_maps, "lights": patched_lights,
                         "lightsUnsaved": light_unsaved,
                         "sceneEnv": patched_scene_env,
                         "sidecar": os.path.relpath(scene_sidecar(scene), REPO) if saved_maps else None,
                         "warnings": warnings})
            return 200, resp
        return 400, {"ok": False, "error": "nothing matched", "warnings": warnings}

    # Patch + write (backup first), only files that actually change.
    patched = []
    for (fname, surf), props in sorted(per_file.items()):
        s = lwos[fname].surface(surf)
        for p, v in props.items():
            SAVELOG.change(G_SURF, f"{fname}:{surf} · {p}: "
                                   f"{_fmtv(_probe(s.get_prop, p))} → {_fmtv(v)}")
            s.set_prop(p, v)
    for (fname, surf), props in sorted(per_file_ext.items()):
        s = lwos[fname].surface(surf)
        was, mask0 = _probe(s.rev_ext) or {}, _probe(s.rev_ext_mask) or 0
        s.set_rev_ext(props)   # RVSF sub-chunk (§1a)
        for p in sorted(props):
            SAVELOG.change(G_SURF, f"{fname}:{surf} · {p}: {_fmtv(was.get(p))} → "
                                   f"{_fmtv(props[p])}  (RVSF {RVSF_BIT.get(p, 0):#06x})")
        mask1 = _probe(s.rev_ext_mask) or 0
        if mask1 != mask0:
            SAVELOG.change(G_SURF, f"{fname}:{surf} · RVSF mask: "
                                   f"{mask0:#06x} → {mask1:#06x}")
    for (fname, surf), st in sorted(per_file_rvsm.items()):
        s = lwos[fname].surface(surf)
        was = (_probe(s.rev_maps) or {}).get("set")
        s.set_rev_maps({"set": st})  # RVSM sub-chunk (§1e)
        SAVELOG.change(G_SURF, f"{fname}:{surf} · RVSM set: {was or '—'} → "
                               f"{st or '— (cleared)'}  (RVSM 0x0001)")
    for fname in sorted({f for (f, _) in per_file} | {f for (f, _) in uv_targets}
                        | {f for (f, _) in per_file_ext} | {f for (f, _) in per_file_rvsm}):
        path = os.path.join(adir, fname)
        new = lwos[fname].serialize()
        cur = open(path, "rb").read()
        if new == cur:
            # The FE's receipt says "saved"; no byte moved. Only the writer can
            # know that, which is the whole reason this log exists.
            SAVELOG.note(f"{fname}: serialized bytes identical — not written")
            continue
        bak = lwopatch.backup(path, scene_backup_dir(scene))
        with open(path, "wb") as f:
            f.write(new)
        SAVELOG.wrote(path, len(cur), f"backup {os.path.relpath(bak, REPO)}")
        patched.append({"file": fname,
                        "surfaces": sorted({s for (f, s) in per_file if f == fname}
                                           | {s for (f, s) in uv_targets if f == fname}
                                           | {s for (f, s) in per_file_ext if f == fname}
                                           | {s for (f, s) in per_file_rvsm if f == fname}),
                        "backup": os.path.relpath(bak, REPO)})

    # Regenerate + install the FLD.
    code, resp = regen_fld(scene, patched)
    if code != 200:
        return code, resp
    resp.update({"maps": saved_maps, "lights": patched_lights,
                 "lightsUnsaved": light_unsaved,
                 "sceneEnv": patched_scene_env,
                 "sidecar": os.path.relpath(scene_sidecar(scene), REPO) if saved_maps else None,
                 "warnings": warnings})
    return 200, resp


def scene_backup_dir(scene):
    """Authoring scenes back up their LWO/LWS sources under
    Authoring/<scene>/.backups; FLD-patched scenes back up the shipping FLD
    under Runtime/SCENES/.backups."""
    if SCENES.get(scene, {}).get("authoring"):
        return os.path.join(scene_authoring_dir(scene), ".backups")
    return FLD_BACKUPS


def list_backups(scene="greets"):
    """Newest-first [{file, target, when}] for the scene's backup dir. For FLD
    scenes only that scene's FLD backups are listed (the dir is shared)."""
    bdir = scene_backup_dir(scene)
    if not os.path.isdir(bdir):
        return []
    only_stem = None if SCENES.get(scene, {}).get("authoring") else scene.upper()
    out = []
    for f in os.listdir(bdir):
        # <stem>.<YYYYmmdd-HHMMSS>[-n].<ext> -> restore target <stem>.<ext>
        m = re.match(r"^(.+)\.(\d{8}-\d{6}(?:-\d+)?)(\.\w+)$", f)
        if not m:
            continue
        # Sort by the FILENAME timestamp (backup creation time) — copy2
        # preserves the source file's mtime, which is when the source was last
        # WRITTEN, not when the backup was taken.
        if only_stem is not None and not m.group(1).upper().startswith(only_stem):
            continue
        out.append({"file": f, "target": m.group(1) + m.group(3),
                    "when": m.group(2)})
    out.sort(key=lambda e: e["when"], reverse=True)
    return out


def do_restore(scene, payload):
    """Bracket the restore with the backend write log — a restore overwrites an
    authoring source and regenerates the FLD, so it hits disk exactly as hard
    as a save and belongs in the same stream."""
    SAVELOG.begin(scene, "restore")
    code = None
    try:
        code, resp = _do_restore(scene, payload)
        return code, resp
    finally:
        SAVELOG.flush(code)


def _do_restore(scene, payload):
    """Restore one backup over its target (backing up the CURRENT file first,
    so a restore is itself undoable). greets targets an authoring LWO/LWS and
    regenerates the FLD; FLD scenes restore the FLD directly."""
    authoring = SCENES.get(scene, {}).get("authoring")
    bdir = scene_backup_dir(scene)
    name = os.path.basename(payload.get("file") or "")
    entry = next((e for e in list_backups(scene) if e["file"] == name), None)
    if entry is None:
        return 404, {"ok": False, "error": f"no such backup '{name}'"}
    target = (os.path.join(scene_authoring_dir(scene), entry["target"]) if authoring
              else os.path.join(RUNTIME, "SCENES", entry["target"]))
    if not os.path.exists(target):
        return 404, {"ok": False, "error": f"target '{entry['target']}' missing"}
    src = os.path.join(bdir, name)
    if open(src, "rb").read() == open(target, "rb").read():
        return 200, {"ok": True, "restored": None,
                     "note": "backup is identical to the current file — nothing to do"}
    before = os.path.getsize(target)
    pre = lwopatch.backup(target, bdir)
    shutil.copy2(src, target)
    SAVELOG.change("restore", f"{entry['target']} ← {name} (taken {entry['when']})")
    SAVELOG.wrote(target, before, f"pre-restore backup {os.path.relpath(pre, REPO)}")
    info = [{"file": entry["target"], "restored_from": name,
             "pre_restore_backup": os.path.relpath(pre, REPO)}]
    if authoring:
        code, resp = regen_fld(scene, info)
        if code != 200:
            return code, resp
    else:
        resp = {"ok": True, "patched": info,
                "fld": os.path.relpath(target, REPO)}
    resp["restored"] = entry["target"]
    return 200, resp


# ── PBR-set catalog + candidate import (browse-not-installed, editor b71) ──
# The picker shows two sections: sets INSTALLED under Runtime/TEXTURES/PBR/
# (pbr_installed_sets) and CANDIDATE sets found in a user-pointed import dir
# (pbr_scan_candidates) that can be installed on choose (pbr_install: copy +
# normalize to <set>/<role>.png, tif/jpg converted via PIL, 16-bit height
# preserved). Default import dir: ~/Downloads WHEN READABLE — macOS TCC often
# denies it to terminal processes ('Operation not permitted', hit live this
# session) — else /tmp. Per-dir read errors are surfaced to the UI, never
# swallowed.

IMPORT_DIR = None          # set by main(); default_import_dir() when unset

ROLE_NAMES = ("albedo", "normal", "height", "roughness", "ao", "metallic")
CAND_EXTS = (".png", ".jpg", ".jpeg", ".tif", ".tiff", ".tga", ".bmp")
# Mirrors the engine's filename classifier (MaterialImport_ClassifyRole /
# classify() token rules), extended for the Polyhaven convention: token list
# per role, FIRST match wins, in the same order; a token matches only on
# separator boundaries (start/end, '_', '-', ' '; digits allowed after:
# normal2, roughness-2k). Polyhaven additions ('sandstone_blocks_05_diff_1k'
# et al): 'diff' -> albedo, 'nor' already covers nor_gl (OpenGL convention —
# the engine's expected one, imported unflipped), 'disp' -> height, and 'arm'
# = the glTF PACKED AO/Roughness/Metallic map (R=AO, G=Roughness, B=Metallic)
# — a pseudo-role the installer SPLITS per channel (see pbr_install). 'spec'
# maps to NO role: the engine has no specular-map slot — the per-material
# specular response dial is the RVSF 'specMul' (skip-listed so a spec file
# can't misclassify).
_ROLE_TOKENS = [
    (None,        ("preview", "thumb", "thumbnail", "sphere", "render",
                   "spec", "specular")),                                   # skip
    ("albedo",    ("basecolor", "base_color", "base-color", "albedo", "diffuse", "diff", "color")),
    ("normal",    ("normal", "nrm", "nor")),
    ("roughness", ("roughness", "rough", "rgh")),
    ("height",    ("height", "disp", "displacement", "bump")),
    ("metallic",  ("metallic", "metalness", "metal")),
    ("ao",        ("ao", "occlusion", "ambientocclusion")),
    ("arm",       ("arm",)),        # packed AO/Rough/Metal — split at install
]


def _token_at(stem, tok):
    """First boundary-respecting occurrence of tok in stem, or -1."""
    pos = 0
    while True:
        pos = stem.find(tok, pos)
        if pos < 0:
            return -1
        left_ok = pos == 0 or stem[pos - 1] in "_- "
        end = pos + len(tok)
        right_ok = end == len(stem) or stem[end] in "_- " or stem[end].isdigit()
        if left_ok and right_ok:
            return pos
        pos = end


_RES_K_RE = re.compile(r"[-_](\d+)k(?=[-_.]|$)")


def _res_k(fname):
    """Resolution-suffix rank: '..._diff_2k.png' -> 2, no suffix -> 0."""
    m = _RES_K_RE.search(os.path.basename(fname).lower())
    return int(m.group(1)) if m else 0


def _classify_full(fname):
    """filename -> (role or None, group-prefix, matched-token). Fixed role
    names ('albedo.png') and suffix conventions ('wall_stone3_albedo.png',
    Polyhaven 'sandstone_blocks_05_diff_1k.png') both classify; the prefix
    (stem before the matched token, resolution suffix stripped) groups loose
    files into a candidate set.

    The RIGHTMOST matching token wins (ties: earlier role row). Role tokens
    are suffixes by convention, while a SET NAME may itself contain a token
    word — 'blue_metal_plate_ao_1k' must classify as ao by its suffix, not as
    metallic via the 'metal' inside the set name (first-row-wins misfiled the
    whole Polyhaven blue_metal_plate set: ao->metallic, arm dropped). NOTE:
    the engine's own MaterialImport_ClassifyRole keeps first-row-wins — it
    only ever sees fixed-name <role>.png files from installed sets (and the
    legacy CLI dir scan), so the divergence is confined to raw library
    filenames, which only this scanner touches."""
    stem, ext = os.path.splitext(os.path.basename(fname))
    if ext.lower() not in CAND_EXTS:
        return None, "", ""
    stem = stem.lower()
    if stem in ROLE_NAMES:
        return stem, "", stem
    best = None                    # (pos, -row, role, tok)
    for row, (role, toks) in enumerate(_ROLE_TOKENS):
        for tok in toks:
            pos = _token_at(stem, tok)
            if pos >= 0 and (best is None or (pos, -row) > (best[0], best[1])):
                best = (pos, -row, role, tok)
    if best is None:
        return None, "", ""
    pos, _nrow, role, tok = best
    pre = _RES_K_RE.sub("", stem[:pos]).rstrip("_- ")
    return (role, pre, tok) if role else (None, pre, tok)


def classify_role_name(fname):
    """filename -> (role or None, group-prefix). See _classify_full."""
    r, pre, _tok = _classify_full(fname)
    return r, pre


def pbr_installed_sets():
    """Sets installed under Runtime/TEXTURES/PBR/<set>/ with their roles."""
    sets = []
    if os.path.isdir(PBR_DIR):
        for name in sorted(os.listdir(PBR_DIR)):
            d = os.path.join(PBR_DIR, name)
            if not os.path.isdir(d):
                continue
            roles = [r for r in ROLE_NAMES
                     if os.path.exists(os.path.join(d, r + ".png"))]
            if roles:
                sets.append({"set": name, "roles": roles,
                             "albedo": (f"TEXTURES/PBR/{name}/albedo.png"
                                        if "albedo" in roles else None)})
    return sets


_PREVIEW_EXTS = (".jpg", ".jpeg", ".png", ".gif", ".bmp")
_PREVIEW_RE = re.compile(r"preview|thumb", re.I)


def _norm_key(s):
    """Normalize for preview<->set-dir matching: lowercase alnum, the FreePBR
    '-bl'/'_bl' suffix stripped ('Iron-Scuffed_bl' ~ 'iron-scuffed-preview2-2')."""
    return re.sub(r"[^a-z0-9]+", "", re.sub(r"[-_]bl$", "", s, flags=re.I).lower())


def _is_preview(fname):
    stem, ext = os.path.splitext(fname)
    return ext.lower() in _PREVIEW_EXTS and _PREVIEW_RE.search(stem) is not None


def _dir_roles(files):
    """(roles{role: filename}, in-dir preview filename or None, extras) for a
    file list. Role matching is case-insensitive (classify lowercases) and maps
    the FreePBR suffix zoo — basecolor/albedo, Normal-ogl -> normal, Height/
    Metallic/Roughness capitalized, '-ao' — plus the Polyhaven one (diff/
    nor_gl/disp/rough/arm) via the shared token classifier. When several files
    map to one role, rank decides: 'disp'/'height' beat 'bump' for the height
    slot (bump is a different, high-frequency signal — only used when nothing
    better exists), and a larger _<N>k resolution suffix beats a smaller one
    (multi-resolution dumps pick the largest; the engine caps at 1024 on load
    anyway). `extras` lists recognized-but-unused map kinds (currently 'spec':
    Polyhaven's own .mtlx ignores its spec map — standard_surface uses constant
    specular=1 + IOR 1.5 with the rough map doing the shaping — so it is
    surfaced in the UI as present-but-unused, never installed as a role)."""
    roles, preview, extras = {}, None, []      # roles: role -> (rank, filename)
    for f in files:
        if preview is None and _is_preview(f):
            preview = f
        r, _pre, tok = _classify_full(f)
        if not r:
            if tok in ("spec", "specular") and "spec" not in extras:
                extras.append("spec")
            continue
        rank = (0 if tok == "bump" else 1, _res_k(f))
        if r not in roles or rank > roles[r][0]:
            roles[r] = (rank, f)
    return {r: f for r, (_rank, f) in roles.items()}, preview, extras


def pbr_parse_mtlx(path):
    """Lightweight MaterialX scrape (plain ElementTree, NO MaterialX library):
    pull the authored constants an install should remember — displacement
    scale (the physical amplitude of the full height range, e.g. Polyhaven's
    0.01), normal-map scale, the standard_surface specular/IOR/metalness
    constants. These land in the installed set's set.meta.json (provenance +
    future seeds for displace_amp / ParallaxScale suggestions). Returns {} on
    any parse trouble — the meta is advisory, never load-bearing.

    Colorspace note (recorded in the meta): .mtlx marks albedo srgb_texture
    and the data maps linear. The engine loads all bytes uniformly (stb) and
    treats albedo as gamma-encoded (the HDR-linear path decodes with a
    gamma-2.0 square; data maps are consumed linearly) — consistent with the
    convention, approximate vs true sRGB. Noted, not fixed here."""
    import xml.etree.ElementTree as ET
    out = {}
    try:
        root = ET.parse(path).getroot()
    except Exception:
        return {}

    def fval(inp):
        v = inp.get("value")
        if v is None:
            return None
        try:
            return float(v)
        except ValueError:
            return None

    for node in root.iter():
        tag = node.tag.split("}")[-1]
        if tag == "displacement":
            for inp in node:
                if inp.get("name") == "scale":
                    v = fval(inp)
                    if v is not None:
                        out["displacementScale"] = v
        elif tag == "normalmap":
            for inp in node:
                if inp.get("name") == "scale":
                    v = fval(inp)
                    if v is not None:
                        out["normalScale"] = v
        elif tag == "standard_surface":
            for inp in node:
                nm, v = inp.get("name"), fval(inp)
                if v is None:
                    continue
                if nm == "specular":
                    out["specular"] = v
                elif nm == "specular_IOR":
                    out["specularIor"] = v
                elif nm == "metalness":
                    out["metalness"] = v
    return out


def _sibling_preview(dirname, parent_previews):
    """Category-level '<set>_preview.jpg' sibling for a set dir, tolerant of
    FreePBR name drift ('iron-scuffed-preview2-2.jpg' vs 'Iron-Scuffed_bl')."""
    key = _norm_key(dirname)
    for pf in parent_previews:
        stem = os.path.splitext(pf)[0]
        cut = _PREVIEW_RE.search(stem)
        p = _norm_key(stem[:cut.start()] if cut else stem)
        if p and key and (p.startswith(key) or key.startswith(p)):
            return pf
    return None


# Category recursion depth: the user's canonical materials root needs 3
# (~/work/materials -> Blender -> <category>-bl -> <set>_bl) and the FreePBR
# library ships one doubly-nested quirk (floors-bl/spaced-tiles1-bl/
# spaced-tiles1-bl/ — the role files sit in a same-named inner dir), so 4.
# The cap exists to keep a mistaken scan of a huge tree bounded.
_MAX_SCAN_DEPTH = 4


def pbr_scan_candidates(root):
    """Scan `root` for CANDIDATE sets. Recognizes BOTH library shapes under
    one root (the user's ~/work/materials):
      - FreePBR:   <root>/Blender/<category>-bl/<set>_bl/<set>_<role>.png —
        role files directly in the set dir;
      - Polyhaven: <root>/polyhaven/<set>_<res>/textures/<set>_<role>_<res>.png
        with the sidecar <set>_<res>.mtlx NEXT TO textures/ — a dir whose role
        files live in a `textures/` child is that ONE set (name from the
        parent dir; the .mtlx references textures/*.jpg/.exr whose extensions
        may differ from the shipped pngs — only its CONSTANTS are scraped, so
        that mismatch is irrelevant);
      - loose file groups in `root` sharing a prefix ('brick_albedo.jpg' +
        'brick_normal.png'), >=2 roles to qualify.
    A dir with neither shape is a CATEGORY and is recursed (depth-capped);
    candidate['category'] is the root-relative parent path ('Blender/
    concrete-bl', 'polyhaven') — the browse UI's grouping. Install names
    strip the resolution suffix (sandstone_blocks_05_1k -> sandstone_blocks_05).
    Ready-made '<set>_preview.jpg' files (in the set dir, or category-level
    siblings with FreePBR name drift) ride each candidate as its thumbnail
    source. Root clutter (.blend/.gltf/.txt/.pdf) has no image-role extension
    and is ignored. Returns (candidates, errors) — read failures (macOS TCC
    EPERM on ~/Downloads etc.) become UI-visible error strings, never a
    silent empty."""
    root = os.path.expanduser(root or "")
    errors = []
    try:
        entries = sorted(os.listdir(root))
    except OSError as e:
        return [], [f"cannot read {root} — {e.strerror or e}: grant access or copy the packs elsewhere (e.g. /tmp)"]
    installed = {s["set"] for s in pbr_installed_sets()}
    out = []

    def find_mtlx(files):
        return next((f for f in files if f.lower().endswith(".mtlx")), None)

    def add(name, src_dir, roles, preview=None, category=None, extras=None,
            mtlx=None, mtlx_dir=None):
        if not roles:
            return
        # Install name: resolution suffix stripped (…_1k -> …), then sanitized.
        safe = re.sub(r"[^A-Za-z0-9._-]+", "_",
                      re.sub(r"[-_]\d+k$", "", name, flags=re.I)) or "set"
        meta = pbr_parse_mtlx(os.path.join(mtlx_dir or src_dir, mtlx)) if mtlx else {}
        out.append({"name": name, "set": safe, "dir": src_dir,
                    "roles": roles,           # role -> source filename
                    "preview": preview,       # {dir, file} or None
                    "category": category,
                    "extras": extras or [],   # present-but-unused kinds (spec)
                    "mtlx": mtlx,             # sidecar MaterialX, if any
                    "mtlxDir": mtlx_dir,      # where the mtlx lives (set dir)
                    "meta": meta,             # scraped mtlx constants
                    "installed": safe in installed})

    def scan_dir(path, name, category, parent_path, parent_previews, depth):
        """Detect `path` as a set (direct role files OR a textures/ child) or
        recurse it as a category. Returns True when handled as a set."""
        try:
            files = sorted(os.listdir(path))
        except OSError as e:
            errors.append(f"cannot read {path} — {e.strerror or e}")
            return True                      # handled (error surfaced)
        roles, prev, extras = _dir_roles(files)
        if roles:                            # set dir, role files inside
            if prev:
                preview = {"dir": path, "file": prev}
            else:
                pf = _sibling_preview(name, parent_previews)
                preview = {"dir": parent_path, "file": pf} if pf else None
            add(name, path, roles, preview, category=category,
                extras=extras, mtlx=find_mtlx(files), mtlx_dir=path)
            return True
        # Polyhaven shape: role files one level down in textures/, the .mtlx
        # sibling here; the whole dir is ONE candidate named after this dir.
        tex = next((f for f in files if f.lower() == "textures"
                    and os.path.isdir(os.path.join(path, f))), None)
        if tex:
            tp = os.path.join(path, tex)
            try:
                tfiles = sorted(os.listdir(tp))
            except OSError as e:
                errors.append(f"cannot read {tp} — {e.strerror or e}")
                return True
            troles, tprev, textras = _dir_roles(tfiles)
            if troles:
                pv = ({"dir": tp, "file": tprev} if tprev else
                      ({"dir": path, "file": prev} if prev else None))
                add(name, tp, troles, pv, category=category,
                    extras=textras, mtlx=find_mtlx(files), mtlx_dir=path)
                return True
        # Category: recurse the subdirectories (depth-capped).
        if depth >= _MAX_SCAN_DEPTH:
            return False
        cat_previews = [f for f in files if _is_preview(f)]
        sub_cat = name if not category else category + "/" + name
        handled = False
        for sub in files:
            sp = os.path.join(path, sub)
            if os.path.isdir(sp):
                handled |= scan_dir(sp, sub, sub_cat, path, cat_previews,
                                    depth + 1)
        return handled

    loose, loose_extras = {}, {}
    root_mtlx = [f for f in entries if f.lower().endswith(".mtlx")
                 and os.path.isfile(os.path.join(root, f))]
    root_previews = [f for f in entries if _is_preview(f)
                     and os.path.isfile(os.path.join(root, f))]
    for name in entries:
        p = os.path.join(root, name)
        if os.path.isdir(p):
            scan_dir(p, name, None, root, root_previews, 1)
        else:
            r, pre, tok = _classify_full(name)
            key = pre or os.path.basename(root.rstrip("/"))
            if r:
                g = loose.setdefault(key, {})
                rank = (0 if tok == "bump" else 1, _res_k(name))
                if r not in g or rank > g[r][0]:
                    g[r] = (rank, name)     # same rank rules as _dir_roles
            elif tok in ("spec", "specular"):
                loose_extras.setdefault(key, []).append("spec")
    for pre, roles in sorted(loose.items()):
        if len(roles) >= 2:       # one stray color file is not a set
            # A root-level .mtlx belongs to the group whose prefix it shares.
            mtlx = next((f for f in root_mtlx
                         if os.path.splitext(f)[0].lower().startswith(pre.lower())), None)
            add(pre, root, {r: f for r, (_rank, f) in roles.items()},
                extras=sorted(set(loose_extras.get(pre, []))), mtlx=mtlx,
                mtlx_dir=root)
    return out, errors


def _pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def _install_role_file(src, out, role, warnings):
    """Copy/convert one source map to <set>/<role>.png. PNG copies verbatim;
    tif/jpg/tga/bmp convert via PIL. 16-bit grayscale sources (the wall_stone3-
    style 16-bit TIF heights) are saved as 16-bit PNG — stb_image reads those;
    note that the engine's height consumer (MakeHeight8) currently reduces to
    8-bit at load, so the extra depth is preserved on disk for the 16-bit-aware
    paths (--nmap_16bit) rather than exploited by height yet."""
    if src.lower().endswith(".png"):
        shutil.copy2(src, out)
        return True
    if Image is None:
        warnings.append(f"{os.path.basename(src)}: PIL not installed — cannot convert to .png, role skipped (pip install pillow)")
        return False
    img = Image.open(src)
    if img.mode in ("I;16", "I;16B", "I;16L", "I"):
        if img.mode != "I;16":
            img = img.convert("I;16")
        img.save(out, format="PNG")   # 16-bit grayscale PNG
    elif img.mode in ("RGBA", "LA", "P", "PA"):
        img.convert("RGBA").save(out, format="PNG")
    elif img.mode == "L":
        img.save(out, format="PNG")
    else:
        if img.mode not in ("RGB",):
            warnings.append(f"{os.path.basename(src)}: mode {img.mode} flattened to 8-bit RGB")
        img.convert("RGB").save(out, format="PNG")
    return True


def pbr_install(src_dir, name, roles, meta=None, mtlx=None):
    """Install a candidate set into Runtime/TEXTURES/PBR/<set>/: role files
    copied + normalized to the fixed <role>.png names, a packed 'arm' map
    (R=AO/G=Rough/B=Metallic) split into whichever of those roles no standalone
    file provided, license/README files copied along, pow2 sanity check
    surfaced as warnings. When the candidate carried .mtlx-scraped constants
    (`meta`, see pbr_parse_mtlx) they are written to <set>/set.meta.json —
    advisory provenance (displacement scale etc.) a later slice can seed the
    editor dials from. Returns (code, resp); on ok the set is immediately
    visible to /api/pbrsets + the engine's dir-per-set loader
    (MaterialImport_ApplyRevMaps)."""
    if not src_dir or not name or not isinstance(roles, dict) or not roles:
        return 400, {"ok": False, "error": "want {dir, name, roles:{role: filename}}"}
    src_dir = os.path.expanduser(src_dir)
    # Same install-name rule as the scanner: resolution suffix stripped
    # (sandstone_blocks_05_1k -> sandstone_blocks_05), then sanitized.
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_",
                  re.sub(r"[-_]\d+k$", "", name, flags=re.I)) or "set"
    dst = os.path.join(PBR_DIR, safe)
    warnings = []
    if os.path.isdir(dst) and any(os.path.exists(os.path.join(dst, r + ".png"))
                                  for r in ROLE_NAMES):
        warnings.append(f"set '{safe}' already installed — overwriting the provided roles")
    os.makedirs(dst, exist_ok=True)
    done, notes = [], []
    roles = dict(roles)
    arm_src = roles.pop("arm", None)   # packed AO/Rough/Metal — split below
    for role, fname in sorted(roles.items()):
        if role not in ALLOWED_ROLES:
            warnings.append(f"unknown role '{role}' — skipped")
            continue
        src = os.path.join(src_dir, os.path.basename(str(fname)))
        if not os.path.isfile(src):
            warnings.append(f"{role}: source {os.path.basename(str(fname))} missing — skipped")
            continue
        out = os.path.join(dst, role + ".png")
        try:
            if not _install_role_file(src, out, role, warnings):
                continue
        except OSError as e:
            warnings.append(f"{role}: {e.strerror or e} — skipped")
            continue
        if Image is not None:
            try:
                with Image.open(out) as im:
                    w, h = im.size
                    # All-zero standalone metallic: same dielectric skip as the
                    # arm.B split below — presence alone enables env-specular.
                    if role == "metallic" and im.convert("L").getextrema()[1] == 0:
                        os.remove(out)
                        notes.append("standalone metallic is all-zero -> NOT installed (dielectric set; avoids the phantom env-specular a black MetallicMap would enable)")
                        continue
                if not (_pow2(w) and _pow2(h)):
                    warnings.append(f"{role}: {w}x{h} is not power-of-two — the engine resamples/caps at load; a pow2 source renders crisper")
            except OSError:
                pass
        done.append(role)
    # Packed 'arm' map (glTF convention: R=AO, G=Roughness, B=Metallic): split
    # the channels into the missing role slots. Standalone ao/rough files are
    # PREFERRED when present (full-range single-channel sources beat the packed
    # 8-bit channel); in practice metallic often exists ONLY as arm.B.
    if arm_src is not None:
        src = os.path.join(src_dir, os.path.basename(str(arm_src)))
        if Image is None:
            warnings.append("arm map present but PIL is not installed — cannot split channels (pip install pillow)")
        elif not os.path.isfile(src):
            warnings.append(f"arm: source {os.path.basename(str(arm_src))} missing — skipped")
        else:
            try:
                with Image.open(src) as arm_img:
                    arm_rgb = arm_img.convert("RGB")
                    for role, ch, chname in (("ao", 0, "R"), ("roughness", 1, "G"),
                                             ("metallic", 2, "B")):
                        if role in done:
                            notes.append(f"arm.{chname} skipped — standalone {role} file installed (higher fidelity than the packed 8-bit channel)")
                            continue
                        chan = arm_rgb.getchannel(ch)
                        # An ALL-ZERO metal channel is a dielectric set
                        # (Polyhaven sandstone/blue_metal_plate): installing a
                        # black metallic.png buys nothing but engine side
                        # effects — a MetallicMap's mere PRESENCE enables the
                        # env-specular compose + auto-defaults env_refl/
                        # env_bake_fix, i.e. a phantom grazing sheen on a
                        # matte set. Skip it (the dial for real spec control
                        # is specMul).
                        if role == "metallic" and chan.getextrema()[1] == 0:
                            notes.append("arm.B all-zero -> metallic NOT installed (dielectric set; avoids the phantom env-specular a black MetallicMap would enable)")
                            continue
                        chan.save(os.path.join(dst, role + ".png"), format="PNG")
                        done.append(role)
                        notes.append(f"arm.{chname} -> {role}.png (split from the packed AO/Rough/Metal map)")
            except OSError as e:
                warnings.append(f"arm: {e.strerror or e} — channels not split")
    if not done:
        return 400, {"ok": False, "error": "no role file could be installed", "warnings": warnings}
    # Keep the pack's license/readme/attribution with the installed set.
    try:
        for f in sorted(os.listdir(src_dir)):
            if re.match(r"(license|licence|readme|attribution|credit)", f, re.I) \
               and os.path.isfile(os.path.join(src_dir, f)):
                shutil.copy2(os.path.join(src_dir, f), os.path.join(dst, f))
    except OSError:
        pass
    # Advisory per-set metadata (mtlx-scraped constants + provenance). The
    # engine ignores it; the picker shows it; a later slice can seed
    # displace_amp / ParallaxScale suggestions from displacementScale.
    if isinstance(meta, dict) and meta:
        meta_out = dict(meta)
        meta_out["source"] = {"dir": src_dir, "mtlx": mtlx}
        meta_out["colorspaceNote"] = (
            "mtlx declares albedo srgb_texture + data maps linear; the engine "
            "loads bytes uniformly and treats albedo as gamma-encoded "
            "(HDR-linear path decodes gamma-2.0), data maps linear — "
            "consistent, approximate vs true sRGB")
        try:
            with open(os.path.join(dst, "set.meta.json"), "w",
                      encoding="utf-8") as f:
                json.dump(meta_out, f, indent=1)
            notes.append("set.meta.json written (mtlx constants: "
                         + ", ".join(f"{k}={v}" for k, v in sorted(meta.items())) + ")")
        except OSError as e:
            warnings.append(f"set.meta.json: {e.strerror or e}")
    return 200, {"ok": True, "set": safe, "roles": sorted(done),
                 "dir": os.path.relpath(dst, REPO),
                 "warnings": warnings, "notes": notes}


def pbr_thumb_png(src, max_dim=256):
    """Downscaled PNG thumbnail bytes for a candidate's source albedo (tif/tga
    sources browsers can't render get converted here), or None."""
    if Image is None or not os.path.isfile(src):
        return None
    try:
        with Image.open(src) as im:
            # Resize needs a displayable mode; a preview only needs 8-bit
            # (16-bit gray -> high byte, palette/other -> RGB).
            if im.mode in ("I;16", "I;16B", "I;16L", "I"):
                # point() on mode 'I' accepts only affine expressions — scale
                # the 16-bit range down with a multiply, not a shift.
                im = im.convert("I").point(lambda v: v * (255.0 / 65535.0)).convert("L")
            elif im.mode not in ("RGB", "RGBA", "L"):
                im = im.convert("RGB")
            im.thumbnail((max_dim, max_dim))
            buf = io.BytesIO()
            im.save(buf, format="PNG")
            return buf.getvalue()
    except (OSError, ValueError):
        return None


def default_import_dir():
    """First READABLE of: ~/work/materials (the user's canonical source root —
    Blender/ = the FreePBR library + polyhaven/ = official Polyhaven layout),
    ~/work/Blender (the library's pre-restructure home, in case of rollback),
    ~/Downloads, /tmp. macOS TCC denies ~/Downloads to many terminal-spawned
    processes (verified live) — probing avoids defaulting the UI onto a dir
    that scans to a permission error."""
    for d in (os.path.expanduser("~/work/materials"),
              os.path.expanduser("~/work/Blender"),
              os.path.expanduser("~/Downloads"), "/tmp"):
        try:
            os.listdir(d)
            return d
        except OSError:
            continue
    return REPO


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=WASM_ROOT, **k)

    def end_headers(self):
        # COOP/COEP: SharedArrayBuffer (pthread build). no-store: the editor
        # iterates on DEMO.wasm + GREETS.FLD; stale caches cost hours.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_json(self, code, obj):
        body = json.dumps(obj, indent=1).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        clean = self.path.split("?")[0]
        mb = re.match(r"^/api/(\w+)/backups$", clean)
        if mb:
            scene = mb.group(1)
            if scene not in SCENES:
                self.send_json(404, {"ok": False, "error": f"unknown scene '{scene}'"})
                return
            self.send_json(200, {"ok": True, "backups": list_backups(scene)[:30]})
            return
        # Installed PBR-set catalog (dir-per-set §1e): the sets already on disk
        # under Runtime/TEXTURES/PBR/<set>/, with which roles each has. The shell
        # renders a preview picker (albedo thumbnail via the /TEXTURES/ live route
        # + role badges) so a set can be eyeballed BEFORE it is applied.
        if clean == "/api/pbrsets":
            self.send_json(200, {"ok": True, "sets": pbr_installed_sets()})
            return
        # CANDIDATE sets (browse-not-installed): scan the import dir (?dir=
        # override, else --import-dir / default_import_dir) for installable
        # sets. Read errors (TCC EPERM etc.) ride the response for the UI.
        if clean == "/api/pbrcandidates":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            d = (q.get("dir") or [None])[0] or IMPORT_DIR or default_import_dir()
            cands, errors = pbr_scan_candidates(d)
            self.send_json(200, {"ok": True, "dir": d, "sets": cands,
                                 "errors": errors, "pil": Image is not None})
            return
        # Thumbnail for a candidate's source map (tif/tga converted to PNG,
        # downscaled) — installed sets use the /TEXTURES/ live route instead.
        if clean == "/api/pbrthumb":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            d = (q.get("dir") or [""])[0]
            f = os.path.basename((q.get("file") or [""])[0])
            png = pbr_thumb_png(os.path.join(os.path.expanduser(d), f)) if d and f else None
            if png is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(png)))
            self.end_headers()
            self.wfile.write(png)
            return
        # Live routes — always the current Runtime/ copy, never the staged one.
        if clean.startswith(LIVE_PREFIXES):
            rel = os.path.normpath(clean.lstrip("/"))
            live = os.path.join(RUNTIME, rel)
            if rel.startswith(("SCENES", "TEXTURES")) \
               and os.path.isfile(live):
                data = open(live, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
        super().do_GET()

    def do_POST(self):
        clean = self.path.split("?")[0]
        # Install a candidate PBR set: {dir, name, roles:{role: filename}} ->
        # copy+normalize into Runtime/TEXTURES/PBR/<set>/ (see pbr_install).
        if clean == "/api/pbrinstall":
            try:
                length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(length) or b"{}")
                with save_lock:
                    code, resp = pbr_install(payload.get("dir"),
                                             payload.get("name"),
                                             payload.get("roles") or {},
                                             meta=payload.get("meta"),
                                             mtlx=payload.get("mtlx"))
            except Exception as e:
                code, resp = 500, {"ok": False, "error": f"{type(e).__name__}: {e}"}
            print(f"[pbr-install] {code}: {resp.get('set')} roles={resp.get('roles')}"
                  + (f" warnings={resp['warnings']}" if resp.get("warnings") else "")
                  + ("" if resp.get("ok") else f" ERROR={resp.get('error')}"))
            self.send_json(code, resp)
            return
        m = re.match(r"^/api/(\w+)/(save|restore)$", clean)
        if not m:
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length) or b"{}")
            with save_lock:
                if m.group(2) == "restore":
                    code, resp = ((404, {"ok": False, "error": f"unknown scene '{m.group(1)}'"})
                                  if m.group(1) not in SCENES else do_restore(m.group(1), payload))
                else:
                    code, resp = do_save(m.group(1), payload)
        except Exception as e:  # report, don't kill the server
            code, resp = 500, {"ok": False, "error": f"{type(e).__name__}: {e}"}
        body = json.dumps(resp, indent=1).encode()
        # Restore-shaped patched entries have no 'surfaces' key — don't let the
        # log line throw after the work is done (it dropped the response).
        print(f"[save] {code}: " + ", ".join(
            f"{p.get('file', '?')}({','.join(p.get('surfaces', []))})"
            for p in resp.get("patched", []))
            + (f" restored={resp['restored']}" if resp.get("restored") else "")
            + (f" warnings={resp['warnings']}" if resp.get("warnings") else "")
            + ("" if resp.get("ok") else f" ERROR={resp.get('error')}"))
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--import-dir", default=None, metavar="DIR",
                    help="directory scanned for CANDIDATE PBR sets (browse-"
                         "not-installed picker). Default: ~/Downloads when "
                         "readable, else /tmp (macOS TCC often denies "
                         "~/Downloads to terminal processes — copy packs to "
                         "/tmp or point the picker's dir field anywhere)")
    args = ap.parse_args()
    global IMPORT_DIR
    IMPORT_DIR = os.path.expanduser(args.import_dir) if args.import_dir \
        else default_import_dir()
    print(f"[server] PBR import dir: {IMPORT_DIR}"
          + ("" if args.import_dir else " (default; override with --import-dir"
             " or the picker's dir field)")
          + ("" if Image is not None else "  [PIL missing: no tif/jpg install"
             " conversion, no candidate thumbnails — pip install pillow]"))
    ensure_lwsread()
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", args.port), Handler) as httpd:
        print(f"[server] {WASM_ROOT}")
        print(f"[server] editor: http://localhost:{args.port}/DEMO.html?editor")
        print("[server] write-back scenes: " + ", ".join(k for k, v in SCENES.items() if v.get("authoring")) + " (source-level), rest via fldpatch")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
