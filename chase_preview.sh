#!/usr/bin/env bash
# Interactive chase review: switch the light-drama variant, regen the FLD
# from CHASE.LWS (lwsread_legacy — the byte-validated pipeline), install it
# into THIS worktree's Runtime, and print the launch line.
#
#   ./chase_preview.sh off                # no light drama (loop + gorge fix only)
#   ./chase_preview.sh runway             # PRIMARY: dim cool runway colonnade
#   ./chase_preview.sh runway+lighthouse  # runway + a warm rotating accent beam
#   ./chase_preview.sh lighthouse         # just the (retuned-down) accent beam
#   ./chase_preview.sh lasers             # legacy sky-fan lasers (tamed; kept for A/B)
#
# Extra args after the variant are passed straight to tools/chase_lights.py
# (e.g. ./chase_preview.sh runway --runway-gain 4.5 --runway-color 150 200 255).
#
# The beams are VOLUMETRIC cones — visible only through the CINEMATIC fog band,
# so launch with --cinematic to judge them. The runway colonnade lines the whole
# lane; the loop is at t~500-580, the gorge at t~1060-1200, open water either side.
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
