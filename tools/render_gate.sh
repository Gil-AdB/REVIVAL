#!/usr/bin/env bash
# Byte-identical render gate for the RenderContext migration
# (docs/RENDER_CONTEXT_PLAN.md). Renders the deterministic deferred test
# scenes and compares each pose-set's md5 to a committed baseline. A Slice-3
# step is byte-clean iff every gate PASSes.
#
# Determinism note: these three scenes are stable run-to-run AND
# threaded==serial. greets is NOT (timing-dependent background lightmap bake)
# and city is unverified — do not add them without re-checking.
#
# Usage:   tools/render_gate.sh            # run from repo root (DEMO in Runtime/)
#          tools/render_gate.sh --update   # reprint current md5s (to re-baseline)
#
# Coverage:
#   mirrortest  — deferred surface kernel + mirror clone + RTT
#   conetest    — DeferredVolumetric cones + DeferredFastFog (fog on)
#   halotest    — DeferredVolumetric omni halos
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/Runtime"
OUT="${TMPDIR:-/tmp}/render_gate"
mkdir -p "$OUT"
cd "$RUN" || { echo "no Runtime/ dir at $RUN"; exit 2; }

# Baselines — committed state. Update with --update after an INTENTED change.
# mirrortest baseline: soa-vertex's d187f9a rebaseline (c340ffa FP-noise drift)
# + ce4f906 mirror clone wall-depth clamp. Verified 2026-07-04 on the merged
# tree — matches soa-vertex's committed golden exactly, so the editor-branch
# work does not perturb mirror output.
BASE_MIRROR="d3a06fb97a01f8c5e250dad7e41f9f4d"
BASE_CONE="8a82e21d53b0d050bbc1220b4fe137f0"
BASE_HALO="a916347df504ab12a3add321747c2f08"

# HEADLESS: every DEMO run uses the SDL dummy driver — no window pops on the
# desktop (the gate gets run a lot, incl. from bisects). Dummy output differs
# from windowed output byte-wise but is deterministic run-to-run, which is all
# a gate needs; the baselines below are DUMMY-mode hashes.
export SDL_VIDEODRIVER=dummy

md5_of() { ls "$@" 2>/dev/null | sort | xargs cat | md5 -q; }

run_mirror() {
  rm -f /tmp/mt_view_*.ppm
  FDS_MIRRORTEST_MULTI_DUMP=1 ./DEMO --scene-mirrortest >/dev/null 2>&1
  md5_of /tmp/mt_view_*.ppm
}
run_cone() {
  rm -f "$OUT"/conetest_*.ppm
  ./DEMO --snapshot=conetest --out="$OUT" --deferred --draw_cones --shadows \
         --fast_fog --fast_fog_density=3 >/dev/null 2>&1
  md5_of "$OUT"/conetest_*.ppm
}
run_halo() {
  rm -f "$OUT"/halotest_*.ppm
  ./DEMO --snapshot=halotest --out="$OUT" --deferred --omni_halo_strength=0.5 >/dev/null 2>&1
  md5_of "$OUT"/halotest_*.ppm
}

M=$(run_mirror); C=$(run_cone); H=$(run_halo)

if [ "${1:-}" = "--update" ]; then
  echo "mirrortest: $M"
  echo "conetest:   $C"
  echo "halotest:   $H"
  exit 0
fi

rc=0
chk() { # name actual baseline
  if [ "$2" = "$3" ]; then echo "  PASS  $1  $2"
  else echo "  FAIL  $1  got $2  want $3"; rc=1; fi
}
echo "render gate:"
chk mirrortest "$M" "$BASE_MIRROR"
chk conetest   "$C" "$BASE_CONE"
chk halotest   "$H" "$BASE_HALO"
[ $rc -eq 0 ] && echo "ALL PASS" || echo "GATE FAILED"
exit $rc
