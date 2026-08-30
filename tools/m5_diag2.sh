#!/bin/bash
# m5_diag2.sh - round 2 M5 diagnostic: FP-environment probe + PIPELINE BISECTION.
# Run from the repo root after `git pull origin fog-wt` and a rebuild:
#   cmake --build build && cmake --install build && bash tools/m5_diag2.sh
# Output: /tmp/m5_diag2.txt (send it back). Compares every dumped G-buffer/Z
# plane of a pinned cam-A snapshot against the committed M2 Max reference, so
# the FIRST divergent pipeline stage is named even if the cause stays unknown.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/m5_diag2.txt
: > "$OUT"
log(){ echo "== $*" | tee -a "$OUT"; }
log "m5_diag2 $(date -u +%Y-%m-%dT%H:%M:%SZ) repo $ROOT @ $(git -C "$ROOT" rev-parse --short HEAD)"
log "fpenv probe"
clang -O2 -o /tmp/fpenv_probe "$ROOT/tools/fpenv_probe.c" -lm && /tmp/fpenv_probe > /tmp/fpenv_this.txt
if diff -q /tmp/fpenv_this.txt "$ROOT/docs/data/fpenv_m2max.txt" >/dev/null 2>&1; then
  echo "fpenv: IDENTICAL to M2 reference" | tee -a "$OUT"
else
  echo "fpenv: DIFFERS:" | tee -a "$OUT"; diff /tmp/fpenv_this.txt "$ROOT/docs/data/fpenv_m2max.txt" | tee -a "$OUT"
fi
log "pinned cam-A snapshot with plane dumps"
rm -rf /tmp/m5d2_out
cd "$ROOT/Runtime" || exit 1
FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
FDS_GREETS_CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \
./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace \
       --force_xres=1920 --force_yres=1080 --snapshot=greets@t=5965 --out=/tmp/m5d2_out >/dev/null 2>&1
( cd /tmp/m5d2_out && for f in $(ls | sort); do [ -f "$f" ] && echo "$(md5 -q "$f")  $f"; done ) | grep -v '\.json$' | tee -a "$OUT" > /tmp/m5d2_planes.txt
log "plane comparison vs M2 reference (first divergent stage)"
if diff /tmp/m5d2_planes.txt "$ROOT/docs/data/planes_m2max.txt" >/dev/null 2>&1; then
  echo "ALL PLANES IDENTICAL to the M2 reference" | tee -a "$OUT"
else
  diff /tmp/m5d2_planes.txt "$ROOT/docs/data/planes_m2max.txt" | tee -a "$OUT"
fi
log "done - send /tmp/m5_diag2.txt"
