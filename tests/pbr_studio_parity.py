#!/usr/bin/env python3
"""Rendering regression test: a CONDUCTOR under a known environment.

This is the repo's first test that asserts on RENDERED PIXELS rather than on
byte-identity, and it exists because byte-identity could not have caught the
class of bug it targets. A metalness workflow kills a surface's diffuse — a
conductor IS its reflection — so if the metalness map is the wrong image, or the
wrong channel of the right image, or the env probe never reaches the material,
the surface goes black or near-black. Every one of those failures produces a
perfectly deterministic, perfectly reproducible frame. A pin would sail through.

WHAT IS FIXED, AND WHY THAT MATTERS
  * The environment is --pbrtest_studio=1, an analytic dome computed from six
    constants in DEMO/PBRTEST.CPP. No capture, no bake order, no probe
    placement, no scene state — the same six cube faces on every machine.
  * The material is Runtime/TEXTURES/PBR/blue_metal_plate, which is IN THIS
    REPO. The test must not depend on an artist's local library.
  * The camera is pinned via FDS_PBRTEST_CAM.
So the only thing that can move these numbers is the shading path.

THE THREE ASSERTIONS, and the bug each one is for:
  mean luminance   the conductor is LIT AT ALL. Goes to ~0 if the metalness map
                   is wrong (an AO map read as metalness pins metalness at ~1
                   over the whole surface and kills all diffuse), if the env
                   probe is missing, or if F0 collapses.
  local contrast   the reflection is SMOOTH. The dome is smooth, so the surface
                   must be. A blown roughness→mip select, or a probe that has
                   picked up the room's checkerboard instead of the dome, shows
                   up here as speckle long before the mean moves. (For scale:
                   the same material in the checkerboard ROOM measures ~13.6%.)
  top/bottom       the reflection is ORIENTED. The dome is bright above and dark
                   below, so upward-facing texels must be brighter than
                   downward-facing ones. Catches a flipped or rotated cube-face
                   basis, and catches the env lobe not being driven by the
                   surface normal at all — both of which leave mean and contrast
                   almost untouched.

── UPDATING THE TOLERANCES WHEN THE LOOK CHANGES ON PURPOSE ─────────────────
These are not sacred. They are a snapshot of a shading path that is under
active development, and an INTENTIONAL improvement (a better BRDF, an energy
fix, a different tonemap) will move them. When that happens:

  1. Be sure the change is intended, and that you can say in one sentence WHY
     each number moved and in which direction you expected it to move. A number
     that moved the wrong way is a bug you have just discovered, not a tolerance
     to widen.
  2. Re-measure:  python3 tests/pbr_studio_parity.py --demo build/DEMO/DEMO \
                      --runtime Runtime --print
     which renders and prints the current values without asserting.
  3. Put the new centres in EXPECT below, keep the tolerances, and say in the
     commit message what moved and why.

DO NOT widen a tolerance to make a red test go green. The tolerances are wide
enough for cross-compiler FP drift and nothing else; if a change exceeds them,
it changed the look, which is exactly what this file is here to notice.

CONTEXT — the number this was built to chase, and why it is not asserted here.
A FreePBR vendor product render of `chipped-paint-metal` (the image that started
this work; shipped as chipped-paint-metal-preview.jpg, and NOT a Blender render —
its XMP says Photoshop CS5, 2018) has a scale-free metal/paint luminance ratio of
1.102 and a local contrast of 2.62%. The same material in this engine measured
0.810 / 13.61% in the pbrtest ROOM and 0.941 / 4.94% under this dome at IDENTICAL
environment energy (both 99.3 mean radiance) — i.e. most of the gap was the
room's shape, not the shading. That comparison is not asserted here because it
needs a material that is not in this repo. It is recorded so the intent is not
lost: see docs and the --pbrtest_studio flag text.
"""

import argparse
import os
import subprocess
import sys
import tempfile

# Sampling window, as fractions of the frame, chosen to sit STRICTLY INSIDE the
# ball at the pinned camera so no background can leak in. Fractions rather than
# pixels so the test survives a resolution change in rev.cfg.
BOX = (0.2917, 0.4271, 0.5741, 0.7778)   # x0, x1, y0, y1

CAM = "-10.6,3.96,-6.0,-10.6,3.96,0"
MATERIAL = "TEXTURES/PBR/blue_metal_plate"
TARGET_SURFACE = "ball_gloss16"

# centre, relative tolerance. Measured 2026-08-11, arm64 / AppleClang 21,
# Release; bit-identical across 3 consecutive runs.
EXPECT = {
    "mean_luma":      (82.470, 0.05),   # +-5%
    "local_contrast": (1.030,  0.30),   # +-30% (second-order stat, noisier)
    "top_bottom":     (3.1154, 0.08),   # +-8%
}


def read_ppm(path):
    """Minimal binary P6 reader -> (w, h, bytes). No PIL, no numpy: this test
    must run anywhere python3 does."""
    with open(path, "rb") as f:
        data = f.read()
    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    if fields[0] != b"P6":
        raise ValueError("not a P6 PPM: %r" % fields[0])
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i:i + w * h * 3]


def metrics(path):
    w, h, px = read_ppm(path)
    x0, x1 = int(BOX[0] * w), int(BOX[1] * w)
    y0, y1 = int(BOX[2] * h), int(BOX[3] * h)
    bw, bh = x1 - x0, y1 - y0
    if bw < 8 or bh < 8:
        raise ValueError("sampling window degenerate at %dx%d" % (w, h))

    # Luminance rows for the window.
    rows = []
    for y in range(y0, y1):
        base = (y * w + x0) * 3
        row = [0.0] * bw
        for k in range(bw):
            o = base + k * 3
            row[k] = 0.2126 * px[o] + 0.7152 * px[o + 1] + 0.0722 * px[o + 2]
        rows.append(row)

    total = sum(sum(r) for r in rows)
    n = bw * bh
    mean = total / n

    # Local contrast: mean |L - mean(3x3 neighbourhood)| / mean, edge-clamped.
    acc = 0.0
    for y in range(bh):
        ym, yp = max(0, y - 1), min(bh - 1, y + 1)
        r0, r1, r2 = rows[ym], rows[y], rows[yp]
        for x in range(bw):
            xm, xp = max(0, x - 1), min(bw - 1, x + 1)
            s = (r0[xm] + r0[x] + r0[xp]
                 + r1[xm] + r1[x] + r1[xp]
                 + r2[xm] + r2[x] + r2[xp])
            acc += abs(r1[x] - s / 9.0)
    local = (acc / n) / mean * 100.0

    third = bh // 3
    top = sum(sum(r) for r in rows[:third]) / (third * bw)
    bot = sum(sum(r) for r in rows[-third:]) / (third * bw)
    return {"mean_luma": mean, "local_contrast": local, "top_bottom": top / bot}


def render(demo, runtime, outdir):
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"      # never pop a window
    env["SDL_AUDIODRIVER"] = "dummy"
    env["FDS_PBRTEST_CAM"] = CAM
    cmd = [demo, "--snapshot=pbrtest@t=100", "--out=" + outdir,
           "--material-import=%s:%s" % (TARGET_SURFACE, MATERIAL),
           "--pbrtest_studio=1"]
    r = subprocess.run(cmd, cwd=runtime, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=300)
    ppm = os.path.join(outdir, "pbrtest_t000100_color.ppm")
    if not os.path.exists(ppm):
        sys.stderr.write(r.stdout.decode("utf-8", "replace")[-4000:])
        raise SystemExit("FAIL: no frame written to %s" % ppm)
    return ppm, r.stdout.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--print", action="store_true",
                    help="measure and print, assert nothing (re-baselining)")
    a = ap.parse_args()

    demo = os.path.abspath(a.demo)
    runtime = os.path.abspath(a.runtime)

    with tempfile.TemporaryDirectory(prefix="pbr_studio_") as td:
        # Two renders, first DISCARDED: the first run of the process in a fresh
        # tree does cold-cache work (texture decode, probe build) and is the
        # known outlier in this repo's measurement harnesses.
        render(demo, runtime, os.path.join(td, "warm"))
        ppm, log = render(demo, runtime, os.path.join(td, "run"))

        if "[PBRTEST-STUDIO]" not in log:
            raise SystemExit("FAIL: --pbrtest_studio=1 did not register a dome; "
                             "the measurement would be against the room.\n"
                             + log[-2000:])
        m = metrics(ppm)

    print("pbr_studio_parity: conductor under the analytic studio dome")
    print("  material %s on '%s'" % (MATERIAL, TARGET_SURFACE))
    bad = []
    for key in ("mean_luma", "local_contrast", "top_bottom"):
        got = m[key]
        want, tol = EXPECT[key]
        lo, hi = want * (1.0 - tol), want * (1.0 + tol)
        ok = lo <= got <= hi
        print("  %-15s %9.4f   expect %.4f +-%.0f%%  [%.4f, %.4f]  %s"
              % (key, got, want, tol * 100, lo, hi, "ok" if ok else "OUT OF RANGE"))
        if not ok:
            bad.append("%s = %.4f, expected %.4f +-%.0f%%" % (key, got, want, tol * 100))

    if a.print:
        print("  (--print: not asserting)")
        return 0
    if bad:
        print("FAILED:")
        for b in bad:
            print("   " + b)
        print("  If this change was INTENTIONAL, read the tolerance-update note "
              "at the top of this file. Do not widen a tolerance to go green.")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
