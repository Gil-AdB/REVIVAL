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

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_ROOT = os.path.join(REPO, "build-wasm", "DEMO")
AUTHORING = os.path.join(REPO, "Authoring", "greets")
BACKUPS = os.path.join(AUTHORING, ".backups")
LWS = "JENINPYR-new-2.LWS"
FLD_INSTALL = os.path.join(REPO, "Runtime", "SCENES", "GREETS.FLD")
LWSREAD = os.path.join(REPO, "tools", "lwsread", "build", "lwsread")

ALLOWED_PROPS = {"baseR", "baseG", "baseB", "diffuse", "specular",
                 "glossiness", "luminosity", "transparency", "reflection"}

save_lock = threading.Lock()


def ensure_lwsread():
    if os.path.exists(LWSREAD):
        return
    src = os.path.join(REPO, "tools", "lwsread")
    print("[server] building tools/lwsread ...")
    subprocess.run(["cmake", "-S", src, "-B", os.path.join(src, "build")],
                   check=True, capture_output=True)
    subprocess.run(["cmake", "--build", os.path.join(src, "build")],
                   check=True, capture_output=True)


def map_surface_name(name):
    """editor surface name -> (lwo_filename or None, surf_name)."""
    name = re.sub(r"::mirUV$", "", name)
    if "::" in name:
        obj, surf = name.split("::", 1)
        surf = re.sub(r"_(body|upper)$", "", surf)
        return obj, surf
    return None, name


def do_save(payload):
    """Apply {"surfaces": {name: {prop: val}}} -> patch LWOs, regen + install FLD."""
    surfaces = payload.get("surfaces") or {}
    if not isinstance(surfaces, dict) or not surfaces:
        return 400, {"ok": False, "error": "no surfaces in payload"}

    lwos = {f: lwopatch.LwoFile(os.path.join(AUTHORING, f))
            for f in sorted(os.listdir(AUTHORING)) if f.endswith(".lwo")}

    warnings = []
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

    if not per_file:
        return 400, {"ok": False, "error": "nothing matched", "warnings": warnings}

    # Patch + write (backup first), only files that actually change.
    patched = []
    for (fname, surf), props in sorted(per_file.items()):
        for p, v in props.items():
            lwos[fname].surface(surf).set_prop(p, v)
    for fname in sorted({f for (f, _) in per_file}):
        path = os.path.join(AUTHORING, fname)
        new = lwos[fname].serialize()
        if new == open(path, "rb").read():
            continue
        bak = lwopatch.backup(path, BACKUPS)
        with open(path, "wb") as f:
            f.write(new)
        patched.append({"file": fname,
                        "surfaces": sorted({s for (f, s) in per_file if f == fname}),
                        "backup": os.path.relpath(bak, REPO)})

    # Regenerate + install the FLD.
    ensure_lwsread()
    tmp_fld = os.path.join(AUTHORING, ".GREETS.tmp.fld")
    r = subprocess.run([LWSREAD, LWS, tmp_fld], cwd=AUTHORING,
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(tmp_fld):
        return 500, {"ok": False, "error": "lwsread failed",
                     "stderr": (r.stderr or r.stdout)[-2000:], "patched": patched}
    shutil.move(tmp_fld, FLD_INSTALL)

    return 200, {"ok": True, "patched": patched,
                 "fld": os.path.relpath(FLD_INSTALL, REPO),
                 "fld_bytes": os.path.getsize(FLD_INSTALL),
                 "warnings": warnings}


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

    def do_GET(self):
        # Live FLD route — always the current Runtime copy, never the staged one.
        if self.path.split("?")[0] == "/SCENES/GREETS.FLD" and os.path.exists(FLD_INSTALL):
            data = open(FLD_INSTALL, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        super().do_GET()

    def do_POST(self):
        if self.path.split("?")[0] != "/api/greets/save":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length) or b"{}")
            with save_lock:
                code, resp = do_save(payload)
        except Exception as e:  # report, don't kill the server
            code, resp = 500, {"ok": False, "error": f"{type(e).__name__}: {e}"}
        body = json.dumps(resp, indent=1).encode()
        print(f"[save] {code}: " + ", ".join(
            f"{p['file']}({','.join(p['surfaces'])})" for p in resp.get("patched", []))
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
        print(f"[server] write-back: {AUTHORING} -> {FLD_INSTALL}")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
