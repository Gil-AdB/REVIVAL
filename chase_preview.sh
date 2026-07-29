#!/usr/bin/env bash
# Interactive chase review: switch the light-drama variant, regen the FLD
# from CHASE.LWS (lwsread_legacy — the byte-validated pipeline), install it
# into THIS worktree's Runtime, and print the launch line.
#
#   ./chase_preview.sh off                # no light drama (loop + gorge fix only)
#   ./chase_preview.sh runway             # PRIMARY: colonnade of ANCHORED, SWEEPING lighthouses
#   ./chase_preview.sh runway+lighthouse  # runway + a warm rotating accent beam
#   ./chase_preview.sh lighthouse         # just the (retuned-down) accent beam
#   ./chase_preview.sh lasers             # legacy sky-fan lasers (tamed; kept for A/B)
#
# Extra args after the variant are passed straight to tools/chase_lights.py
# (e.g. ./chase_preview.sh runway --runway-gain 4.5 --runway-turns 2 --runway-fixed-alt,
#  or ./chase_preview.sh runway --runway-model proc  for the procedural-tower fallback).
#
# The PRIMARY runway variant plants a real red/white-striped CC0 lighthouse
# (Authoring/chase/lighthouse.lwo) at each emitter, seated TERRAIN-AWARE (base
# buried under the water / inside the island rock), and PARENTS two coloured
# volumetric spots to a spinning null: the beam SWEEPS and its colour SHIFTS
# (anti-phase LgtIntensity envelopes crossfade a palette pair per beacon; all
# phases staggered down the lane). The beams are VOLUMETRIC cones — visible
# only through the CINEMATIC fog band, so launch with --cinematic to judge
# them; the towers themselves render in plain --deferred too. Beacons line the
# open water; the loop is t~500-580, the gorge t~1060-1200 (kept clear).
#
# COMBAT (ship2 fires at Ship1) is an orthogonal runtime flag, not baked into the
# FLD — add --flags-file=PRESETS/chase-combat.flags to any launch below to see it.
set -euo pipefail
cd "$(dirname "$0")"

VARIANT="${1:-}"; shift || true
case "$VARIANT" in
    off)                ARGS=(--clear) ;;
    runway)             ARGS=(--runway) ;;
    runway+lighthouse)  ARGS=(--runway --lighthouse) ;;
    lighthouse)         ARGS=(--lighthouse) ;;
    lasers)             ARGS=(--lasers 8) ;;
    *) echo "usage: $0 {off|runway|runway+lighthouse|lighthouse|lasers} [chase_lights.py extra args]"; exit 2 ;;
esac

python3 tools/chase_lights.py "${ARGS[@]}" "$@"
( cd Authoring/chase \
  && ../../tools/lwsread/build/lwsread_legacy CHASE.LWS CHASE.FLD \
  && cp CHASE.FLD ../../Runtime/SCENES/CHASE.FLD )
echo ""
echo "installed light variant '$VARIANT' into $(pwd)/Runtime/SCENES/CHASE.FLD"
echo "(the ship gorge-clip fix is baked into CHASE.LWS — tools/chase_gorge_fix.py)"
echo ""
echo "FLY IT (headless-safe review; run from Runtime/):"
echo "  loop:        cd Runtime && ./DEMO --scene-chase --deferred --chase-start=460"
echo "  runway/gorge: cd Runtime && ./DEMO --scene-chase --deferred --cinematic --chase-start=340"
echo "  full chase:  cd Runtime && ./DEMO --scene-chase --deferred --cinematic"
echo "  + COMBAT:    add  --flags-file=PRESETS/chase-combat.flags  to any of the above"
echo "  + PROFILER:  add  --profiler   (per-section ms/FPS overlay in the corner)"
echo "  + CAM DUMP:  add  --chase_cam_dump   ([CHASECAM] per-frame camera/ship trace on stderr)"
