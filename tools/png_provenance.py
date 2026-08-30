#!/usr/bin/env python3
"""Print the groundwork-provenance record embedded in a PNG (or a PPM's sidecar).

Usage:
    tools/png_provenance.py <image> [<image> ...]
    tools/png_provenance.py --short <image> ...     one line per image
    tools/png_provenance.py --repro <image>         print a runnable repro command

Exit 1 when any named image carries no provenance, so this doubles as a check
in a script: `tools/png_provenance.py docs/img/foo/*.png >/dev/null` fails on
the first unprovenanced render.
"""

from __future__ import annotations

import json
import os
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from provenance import provenance_of  # noqa: E402


def _short(path, rec):
    if rec is None:
        return f"{path}: NO PROVENANCE"
    if rec.get("schema") == "revival/derived-provenance":
        srcs = ", ".join(os.path.basename(i["path"]) for i in rec.get("inputs", []))
        return f"{path}: derived by {rec['derived']['tool']} from [{srcs}]"
    b = rec.get("build", {})
    cam = (rec.get("camera") or {}).get("greets_cam", "-")
    sha = b.get("git_describe") or b.get("git_sha", "?")
    return (f"{path}: {rec.get('scene', '?')} t={rec.get('t', '?')} "
            f"{sha[:12]}{'+dirty' if b.get('git_dirty') and '+dirty' not in sha[:12] else ''} "
            f"cam={cam} flags={len(rec.get('flags', []))}")


def _repro(rec):
    """The command line that made this image, as text you can paste."""
    if rec is None or rec.get("schema") == "revival/derived-provenance":
        return None
    env = rec.get("env", {}) or {}
    parts = ["SDL_VIDEODRIVER=dummy", "SDL_AUDIODRIVER=dummy"]
    parts += [f"{k}={shlex.quote(v)}" for k, v in sorted(env.items())]
    parts += [shlex.quote(a) for a in rec.get("argv", [])]
    head = f"cd {shlex.quote(rec.get('cwd', '.'))} && " + " ".join(parts)
    b = rec.get("build", {})
    note = (f"# built from {b.get('git_describe', '?')} on {b.get('git_branch', '?')}"
            f" in {b.get('source_dir', '?')}")
    if b.get("git_dirty"):
        note += "\n# WARNING: tree was DIRTY at configure time — the SHA alone does not describe this binary"
    return note + "\n" + head


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    short = "--short" in argv
    repro = "--repro" in argv
    if not args:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    missing = 0
    for path in args:
        rec = provenance_of(path)
        if rec is None:
            missing += 1
        if short:
            print(_short(path, rec))
        elif repro:
            r = _repro(rec)
            print(r if r else f"# {path}: no direct-render provenance")
        else:
            print(f"=== {path} ===")
            print(json.dumps(rec, indent=2) if rec is not None else "NO PROVENANCE")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
