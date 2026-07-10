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
import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import threading

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
    "city":     {"authoring": False},
    "crash":    {"authoring": False},
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

ALLOWED_PROPS = {"baseR", "baseG", "baseB", "diffuse", "specular",
                 "glossiness", "luminosity", "transparency", "reflection",
                 # smoothAngle is a NATIVE LWO/FLD field (MaxSmoothingAngle +
                 # the Surf_Smoothing flag), so on FLD-patched scenes (city/
                 # crash) it round-trips through fldpatch like diffuse/specular.
                 # But the LWO patcher (authoring scenes: greets/chase/fountain)
                 # has no smoothing chunk, so there it is peeled to the sidecar
                 # instead — see split_surface_sidecar_keys / SMOOTH_SIDECAR.
                 "smoothAngle"}
# Engine-only per-material dials with no LWO/FLD field — persist as sidecar
# surface|prop|value lines (MaterialImport_ApplySidecar sets them at init).
# refractIor: per-surface glass-refraction IOR (0 = unset -> the global
# glass_refract_ior render knob).
SURF_SIDECAR_KEYS = {"aoStrength", "parallaxScale", "normalFlip", "tintR", "tintG", "tintB", "refractive", "refractIor", "envRefl"}
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
    become empty). Runtime '#k' instance-split names collapse to the base
    surface (splits are live-only). Returns a summary list."""
    if not surfaces:
        return []
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    # Authoring scenes have no LWO smoothing chunk — route smoothAngle to the
    # sidecar there (see SMOOTH_SIDECAR); FLD scenes patch it natively.
    keys = SURF_SIDECAR_KEYS | (SMOOTH_SIDECAR if SCENES[scene].get("authoring") else set())
    saved = []
    for name in list(surfaces):
        props = surfaces[name]
        if not isinstance(props, dict):
            continue
        side = {k: props.pop(k) for k in list(props) if k in keys}
        # Strip the WHOLE trailing (#k)+ chain: the runtime split renames the
        # primary "<name>#1" too, and a re-split of a part can chain suffixes
        # ("momy#2#2") — every one of them persists onto the base surface.
        base = re.sub(r"(#\d+)+$", "", name)
        for k, v in side.items():
            entries[(base, k)] = f"{float(v):.6g}"
            saved.append({"surface": base, "key": k})
        if not props:
            del surfaces[name]
    if saved:
        write_sidecar(sidecar, entries)
    return saved
ALLOWED_ROLES = {"albedo", "normal", "height", "roughness", "ao", "metallic"}

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
        out[re.sub(r"(#\d+)+$", "", name)] = (proj, float(uv["uvScaleX"]),
                                              float(uv["uvScaleY"]), float(uv["uvScaleZ"]),
                                              int(uv["uvAxis"]))
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
    # Runtime instance-split parts ("momy#1"/"momy#2", possibly chained) are
    # live-only clones of ONE authored surface — patch the base. Note the
    # split now renames the primary to "#1" too, so without this collapse the
    # primary's own edits would stop reaching the .lwo.
    name = re.sub(r"(#\d+)+$", "", name)
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
    lines = ["# Editor overrides — written by tools/editor_server.py (editor",
             "# \"Save\"), loaded at scene init by MaterialImport_ApplySidecar.",
             "# Format: surface|role|path-relative-to-Runtime (PBR map)",
             "#         surface|prop|value                    (numeric override)", ""]
    for (surface, key), value in sorted(entries.items()):
        lines.append(f"{surface}|{key}|{value}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def save_maps(scene, maps, warnings):
    """{"<surface>": {"<role>": {"filename": ..., "data": base64}}} -> write the
    bytes under Runtime/TEXTURES/PBR/ and update the scene sidecar. Returns the
    list of written entries. Deterministic filenames so a re-import of the same
    slot overwrites instead of accumulating files."""
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
            with open(os.path.join(PBR_DIR, safe), "wb") as f:
                f.write(data)
            rel = f"TEXTURES/PBR/{safe}"
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


LIGHT_KEYS = {"r", "g", "b", "intensity", "range"}
# Engine-only per-light extensions: no LWS/FLD field exists, so they persist
# as sidecar "light:<i>|<key>|<value>" lines (applied at scene init by
# MaterialImport_ApplySidecar).
LIGHT_SIDECAR_KEYS = {"flareScale"}


def split_light_sidecar_keys(scene, lights, warnings):
    """Strip LIGHT_SIDECAR_KEYS out of the save payload's light dicts and
    persist them as sidecar light: lines. Mutates `lights` (drops entries
    that become empty). Returns a summary list."""
    if not lights:
        return []
    sidecar = scene_sidecar(scene)
    entries = read_sidecar(sidecar)
    saved = []
    for idx_s in list(lights):
        props = lights[idx_s]
        if not isinstance(props, dict):
            continue
        side = {k: props.pop(k) for k in list(props) if k in LIGHT_SIDECAR_KEYS}
        for k, v in side.items():
            entries[(f"light:{int(idx_s)}", k)] = f"{float(v):.6g}"
            saved.append({"light": int(idx_s), "key": k})
        if not props:
            del lights[idx_s]
    if saved:
        write_sidecar(sidecar, entries)
    return saved


def patch_lws_lights(scene, lights, warnings):
    """{"<index>": {r,g,b,intensity,range}} -> patch the i-th AddLight block of
    the scene's LWS (LWSC v1 is line-based text: LightColor R G B /
    LgtIntensity F / LightRange F). Index order == the engine's
    Omni_SceneAuthored order == AddLight file order. Returns list of patched
    entries; writes + backs up the LWS only if something changed."""
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
            continue
        bad = set(props) - LIGHT_KEYS
        if bad:
            warnings.append(f"light {idx}: unknown keys {sorted(bad)} — skipped")
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
                lines[i] = f"LightColor {r} {g} {b}"
                done |= {"r", "g", "b"} & set(props)
            elif stripped.startswith("LgtIntensity ") and "intensity" in props:
                lines[i] = f"LgtIntensity {float(props['intensity']):.6f}"
                done.add("intensity")
            elif stripped.startswith("LightRange ") and "range" in props:
                lines[i] = f"LightRange {float(props['range']):.6f}"
                done.add("range")
        missing = set(props) - done
        if missing:
            warnings.append(f"light {idx}: keys {sorted(missing)} not found in its "
                            f"AddLight block (envelope-animated?) — skipped those")
        if done:
            patched.append({"index": idx, "keys": sorted(done)})
    if patched:
        new = "\n".join(lines)
        if new != open(path, encoding="latin-1").read():
            bak = lwopatch.backup(path, scene_backup_dir(scene))
            with open(path, "w", encoding="latin-1") as f:
                f.write(new)
            for p in patched:
                p["file"] = lws_name
                p["backup"] = os.path.relpath(bak, REPO)
        else:
            patched = []   # values identical — nothing actually changed
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
    shutil.move(tmp_fld, fld_install)
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
        # Editor names are base-collapsed already; strip a runtime (#k)+
        # instance-split suffix chain (live-only clones, incl. the renamed
        # "#1" primary — the FLD has one surface).
        base = re.sub(r"(#\d+)+$", "", name)
        if base != name:
            warnings.append(f"'{name}' is a runtime instance split — patched the "
                            f"base surface '{base}' (splits are live-only)")
        n = fld.patch_material(base, props)
        if n == 0:
            warnings.append(f"'{name}': no material record in {os.path.basename(fld_path)} — skipped")
        else:
            patched_surfaces.append({"surface": base, "records": n, "props": sorted(props)})

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

    wrote = None
    if patched_surfaces or patched_lights:
        if bytes(fld.data) != open(fld_path, "rb").read():
            bak = lwopatch.backup(fld_path, FLD_BACKUPS)
            fld.save(fld_path)
            wrote = {"file": os.path.basename(fld_path),
                     "surfaces": [p["surface"] for p in patched_surfaces],
                     "backup": os.path.relpath(bak, REPO)}
        else:
            warnings.append("values identical to the FLD — nothing written")

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
    """Apply {"surfaces": {...}, "maps": {...}, "lights": {...}}.
    greets: patch LWOs + LWS, regen + install the FLD.
    other scenes (no pinned sources): surfaces persist as sidecar prop lines,
    lights are live-only (warned). Maps go to the scene sidecar either way."""
    if scene not in SCENES:
        return 404, {"ok": False, "error": f"unknown scene '{scene}'"}
    surfaces = payload.get("surfaces") or {}
    maps = payload.get("maps") or {}
    lights = payload.get("lights") or {}
    if not isinstance(surfaces, dict) or not isinstance(lights, dict) \
       or (not surfaces and not maps and not lights):
        return 400, {"ok": False, "error": "no surfaces, maps, or lights in payload"}

    warnings = []
    saved_maps = save_maps(scene, maps, warnings)
    # Engine-only keys (light flareScale, surface aoStrength/parallaxScale) →
    # sidecar, for every scene type; what remains goes to the LWS/LWO/FLD
    # patchers below.
    saved_light_side = split_light_sidecar_keys(scene, lights, warnings)
    if saved_light_side:
        warnings.append(f"{len(saved_light_side)} light key(s) → sidecar (engine-only, no LWS/FLD field)")
    saved_surf_side = split_surface_sidecar_keys(scene, surfaces, warnings)
    if saved_surf_side:
        warnings.append(f"{len(saved_surf_side)} surface key(s) → sidecar (engine-only, no LWO/FLD field)")
    uv_by_name = pop_uv_props(surfaces, warnings)

    if not SCENES[scene]["authoring"]:
        return do_save_fld(scene, surfaces, lights, uv_by_name, saved_maps, warnings)

    patched_lights = patch_lws_lights(scene, lights, warnings)
    if not surfaces and not uv_by_name:
        if patched_lights:
            # Lights changed -> the FLD must be regenerated (it embeds them).
            code, resp = regen_fld(scene, [])
            if code != 200:
                return code, resp
            resp.update({"maps": saved_maps, "lights": patched_lights,
                         "warnings": warnings})
            return 200, resp
        # Maps-only save: no FLD regen needed (map paths live in the sidecar;
        # the FLD doesn't reference them).
        return 200, {"ok": True, "patched": [], "maps": saved_maps,
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
        if re.search(r"(#\d+)+$", re.sub(r"::mirUV$", "", name)):
            warnings.append(f"'{name}' is a runtime instance split — patching the "
                            f"base surface '{surf}' (splits are live-only)")
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
    for (fname, surf), uv in sorted(uv_targets.items()):
        lwos[fname].surface(surf).set_uv_mapping(*uv)

    if not per_file and not uv_targets:
        if saved_maps or patched_lights:   # surface edits all missed, but these landed
            code, resp = (regen_fld(scene, []) if patched_lights
                          else (200, {"ok": True, "patched": []}))
            if code != 200:
                return code, resp
            resp.update({"maps": saved_maps, "lights": patched_lights,
                         "sidecar": os.path.relpath(scene_sidecar(scene), REPO) if saved_maps else None,
                         "warnings": warnings})
            return 200, resp
        return 400, {"ok": False, "error": "nothing matched", "warnings": warnings}

    # Patch + write (backup first), only files that actually change.
    patched = []
    for (fname, surf), props in sorted(per_file.items()):
        for p, v in props.items():
            lwos[fname].surface(surf).set_prop(p, v)
    for fname in sorted({f for (f, _) in per_file} | {f for (f, _) in uv_targets}):
        path = os.path.join(adir, fname)
        new = lwos[fname].serialize()
        if new == open(path, "rb").read():
            continue
        bak = lwopatch.backup(path, scene_backup_dir(scene))
        with open(path, "wb") as f:
            f.write(new)
        patched.append({"file": fname,
                        "surfaces": sorted({s for (f, s) in per_file if f == fname}
                                           | {s for (f, s) in uv_targets if f == fname}),
                        "backup": os.path.relpath(bak, REPO)})

    # Regenerate + install the FLD.
    code, resp = regen_fld(scene, patched)
    if code != 200:
        return code, resp
    resp.update({"maps": saved_maps, "lights": patched_lights,
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
    pre = lwopatch.backup(target, bdir)
    shutil.copy2(src, target)
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
    args = ap.parse_args()
    ensure_lwsread()
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", args.port), Handler) as httpd:
        print(f"[server] {WASM_ROOT}")
        print(f"[server] editor: http://localhost:{args.port}/DEMO.html?editor")
        print("[server] write-back scenes: " + ", ".join(k for k, v in SCENES.items() if v.get("authoring")) + " (source-level), rest via fldpatch")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
