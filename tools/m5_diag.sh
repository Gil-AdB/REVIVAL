#!/bin/bash
# m5_diag.sh - one-shot diagnostic for the M5 missing-polys report (ledger cb995428c637).
# Run from anywhere inside the repo: bash tools/m5_diag.sh
# Collects machine/compiler/build info, runs the render gate, and a 24-run
# determinism loop at the greets cam-A pin. Output: /tmp/m5_diag.txt (send it back).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/m5_diag.txt
: > "$OUT"
log(){ echo "== $*" | tee -a "$OUT"; }
run(){ echo "-- $*" >> "$OUT"; "$@" >> "$OUT" 2>&1; }
log "m5_diag $(date -u +%Y-%m-%dT%H:%M:%SZ)  repo $ROOT"
run sw_vers
run uname -a
run sysctl -n machdep.cpu.brand_string
run git -C "$ROOT" log --oneline -1
run git -C "$ROOT" status --short
log "compilers"
run clang --version
run xcrun clang --version
run which clang cc c++
log "build cache (which compiler built this binary)"
grep -E 'CMAKE_(C|CXX)_COMPILER:|CMAKE_BUILD_TYPE|OSX_ARCH' "$ROOT"/build/CMakeCache.txt >> "$OUT" 2>&1 || echo "no build/CMakeCache.txt" >> "$OUT"
run ls -la "$ROOT/Runtime/DEMO"
log "render gate (4 rows, known hashes; PASS = byte-identical to the dev box)"
( cd "$ROOT" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy tools/render_gate.sh ) >> "$OUT" 2>&1
log "24-run determinism loop, greets cam A t=5965 (one hash x24 = deterministic; several = race)"
CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
cd "$ROOT/Runtime" || exit 1
for i in $(seq 1 24); do
  rm -rf /tmp/m5diag_run
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="$CAM" \
    ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace \
           --force_xres=1920 --force_yres=1080 --snapshot=greets@t=5965 --out=/tmp/m5diag_run >/dev/null 2>&1
  md5 -q /tmp/m5diag_run/greets_t005965_color.ppm 2>>"$OUT"
done | sort | uniq -c | tee -a "$OUT"
cp /tmp/m5diag_run/greets_t005965_color.ppm /tmp/m5diag_camA.ppm 2>/dev/null
cp /tmp/m5diag_run/greets_t005965_color.json /tmp/m5diag_camA.json 2>/dev/null
log "done. Send back: /tmp/m5_diag.txt (and /tmp/m5diag_camA.ppm if the gate failed)"
