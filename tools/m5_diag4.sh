#!/bin/bash
# m5_diag4.sh - round 4: the M5 ceiling defect is THREE MISSING OMNI LIGHTS, not
# geometry. The dev box proved (rev-knifedge, 2026-08-31) that at greets cam A
# t=5965 the M5's G-buffer is IDENTICAL to the dev box's on every divergent
# ceiling pixel - same z16, same matID, same texel - and that skipping exactly
# the three cyan mech omnis (seq 114/115/116, colour (0,128,255), ISize 0.75,
# IRange 30) locally reproduces the M5's ceiling to ~1/255. So the whole
# divergence lives in Render_DeferredLighting's LIGHT SELECTION for that region.
#
# This script runs the arms that can RECOVER a wrongly-dropped light without any
# code change, and prints the ceiling-band mean RGB for each. Send the table back.
#
#   ./DEMO must be installed in Runtime/ (cmake --build build && cmake --install build)
#   Run from the repo root:  bash tools/m5_diag4.sh
#
# DEV-BOX REFERENCE (M2 Max, same recipe, band = rows 5..40, cols 600..1300):
#   baseline                 85.2 133.5 139.0      <- correct, lights present
#   --prof_no_lights         80.9  61.8  46.9      <- all omnis off
#   skip lights 114/115/116  85.2  65.0  46.9      <- the three cyan mech omnis off
#   M5 (his disp_color.ppm)  85.3  65.8  47.8      <- matches the skip-3 arm
#
# READ THE TABLE LIKE THIS: any arm whose M5 numbers jump from ~(85,66,48) to
# ~(85,133,139) NAMES the gate that dropped the lights. --light_rect_exact is the
# prime suspect: the shipping small-angle light screen-rect is DOCUMENTED as
# non-conservative (FeatureFlags.def) and its own audit reports up to 21% of
# (light x tile) pairs dropped with up to 30.7% of full brightness recoverable.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/m5_diag4.txt
: > "$OUT"
log(){ echo "$*" | tee -a "$OUT"; }
CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
BASE="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --force_xres=1920 --force_yres=1080"
log "m5_diag4 $(date -u +%Y-%m-%dT%H:%M:%SZ) @ $(git -C "$ROOT" rev-parse --short HEAD)"
log "arm                              ceiling band mean R G B"
cd "$ROOT/Runtime" || exit 1
run_arm() {
  local label="$1"; shift
  rm -rf /tmp/m5d4_out
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="$CAM" \
    ./DEMO $BASE "$@" --snapshot=greets@t=5965 --out=/tmp/m5d4_out >/dev/null 2>&1
  local ppm=/tmp/m5d4_out/greets_t005965_color.ppm
  if [ ! -f "$ppm" ]; then log "$(printf '%-32s' "$label") FAILED (no snapshot)"; return; fi
  python3 - "$ppm" "$label" <<'PY' | tee -a "$OUT"
import numpy as np, sys
f=open(sys.argv[1],'rb'); f.readline(); l=f.readline()
while l.startswith(b'#'): l=f.readline()
w,h=map(int,l.split()); f.readline()
a=np.frombuffer(f.read(w*h*3),dtype=np.uint8).reshape(h,w,3)
m=a[5:40,600:1300].reshape(-1,3).mean(axis=0)
print("%-32s %6.1f %6.1f %6.1f" % (sys.argv[2], m[0], m[1], m[2]))
PY
}
run_arm "baseline"                                    # expect the M5's ~85/66/48
run_arm "--light_rect_exact"     --light_rect_exact   # PRIME SUSPECT
run_arm "--no-deferred_zcull"    --no-deferred_zcull  # drop the per-tile depth cull
run_arm "--light_rect_exact --no-deferred_zcull" --light_rect_exact --no-deferred_zcull
run_arm "--no-shadows"           --no-shadows         # confirm shadows are not it
run_arm "--no-shadow_polyid"     --no-shadow_polyid
run_arm "--prof_no_lights"       --prof_no_lights     # floor of the scale
run_arm "--no-greets_mirror"     --no-greets_mirror   # kills the mirror-id gate entirely
log ""
log "also useful, one line: FDS_LIGHTRECT_AUDIT=1 ./DEMO $BASE --light_rect_exact --snapshot=greets@t=5965 --out=/tmp/lr 2>&1 | grep LIGHTRECT"
log "done - send /tmp/m5_diag4.txt"
