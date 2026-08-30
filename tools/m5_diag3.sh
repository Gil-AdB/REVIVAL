#!/bin/bash
# m5_diag3.sh - round 3: BAKE vs TRANSFORM bisection, and ship the planes back.
# The displaced-arm z16 differs between machines (m5_diag2). This renders the
# BARE arm (no --greets-displace): if bare z16 MATCHES the M2 reference, the
# authored-geometry transform/raster is identical and the divergence lives in
# the DISPLACEMENT BAKE; if bare differs too, it is the transform/raster itself.
# Also copies the M5 planes (bare + displaced) to iCloud Drive for the dev box.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/m5_diag3.txt
: > "$OUT"
log(){ echo "== $*" | tee -a "$OUT"; }
CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
log "m5_diag3 $(date -u +%Y-%m-%dT%H:%M:%SZ) @ $(git -C "$ROOT" rev-parse --short HEAD)"
cd "$ROOT/Runtime" || exit 1
log "BARE arm (no displacement)"
rm -rf /tmp/m5d3_out
FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
FDS_GREETS_CAM="$CAM" ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
  --force_xres=1920 --force_yres=1080 --snapshot=greets@t=5965 --out=/tmp/m5d3_out >/dev/null 2>&1
( cd /tmp/m5d3_out && for f in $(ls | sort); do [ -f "$f" ] && echo "$(md5 -q "$f")  $f"; done ) | grep -v '\.json$' | tee -a "$OUT" > /tmp/m5d3_planes.txt
if diff /tmp/m5d3_planes.txt "$ROOT/docs/data/planes_bare_m2max.txt" >/dev/null 2>&1; then
  echo "BARE: ALL PLANES IDENTICAL to M2 -> the divergence is in the DISPLACEMENT BAKE" | tee -a "$OUT"
else
  echo "BARE: DIFFERS -> the transform/raster itself diverges:" | tee -a "$OUT"
  diff /tmp/m5d3_planes.txt "$ROOT/docs/data/planes_bare_m2max.txt" | tee -a "$OUT"
fi
log "copying planes to iCloud Drive for the dev box"
IC="$HOME/Library/Mobile Documents/com~apple~CloudDocs"
mkdir -p "$IC/m5planes" 2>/dev/null
cp /tmp/m5d3_out/greets_t005965_depth.z16 "$IC/m5planes/bare_depth.z16" 2>>"$OUT"
cp /tmp/m5d2_out/greets_t005965_depth.z16 "$IC/m5planes/disp_depth.z16" 2>>"$OUT" || echo "(re-run tools/m5_diag2.sh if /tmp/m5d2_out is gone)" | tee -a "$OUT"
cp /tmp/m5d2_out/greets_t005965_mat.u32  "$IC/m5planes/disp_mat.u32"  2>>"$OUT"
cp /tmp/m5d2_out/greets_t005965_color.ppm "$IC/m5planes/disp_color.ppm" 2>>"$OUT"
ls -la "$IC/m5planes" >> "$OUT" 2>&1
log "done - send /tmp/m5_diag3.txt (planes go via iCloud)"
