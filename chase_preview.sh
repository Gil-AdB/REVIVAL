#!/usr/bin/env bash
# Interactive chase review: switch the light-drama variant, regen the FLD
# from CHASE.LWS (lwsread_legacy — the byte-validated pipeline), install it
# into THIS worktree's Runtime, and print the launch line.
#
#   ./chase_preview.sh off          # no drama (loop fix only) — restores base LWS
#   ./chase_preview.sh lighthouse   # rotating warm beam on the island
#   ./chase_preview.sh lasers       # crisscross coloured beams over the passage
#   ./chase_preview.sh combined     # lighthouse + lasers + realigned canyon spots
#   ./chase_preview.sh realign      # only re-aim the two existing canyon spots
#
# Extra args after the variant are passed straight to tools/chase_lights.py
# (e.g. ./chase_preview.sh combined --lh-gain 0.5 --laser-gain 0.6).
#
# The beams read through the CINEMATIC profile's fog band — launch with
# --cinematic to judge them. Loop is at t~500-580, island stretch t~1200+.
set -euo pipefail
cd "$(dirname "$0")"

VARIANT="${1:-}"; shift || true
case "$VARIANT" in
    off)        ARGS=(--clear) ;;
    lighthouse) ARGS=(--lighthouse) ;;
    lasers)     ARGS=(--lasers 8) ;;
    combined)   ARGS=(--lighthouse --lasers 8 --realign) ;;
    realign)    ARGS=(--realign) ;;
    *) echo "usage: $0 {off|lighthouse|lasers|combined|realign} [chase_lights.py extra args]"; exit 2 ;;
esac

python3 tools/chase_lights.py "${ARGS[@]}" "$@"
( cd Authoring/chase \
  && ../../tools/lwsread/build/lwsread_legacy CHASE.LWS CHASE.FLD \
  && cp CHASE.FLD ../../Runtime/SCENES/CHASE.FLD )
echo ""
echo "installed variant '$VARIANT' into $(pwd)/Runtime/SCENES/CHASE.FLD"
echo "run it (loop):    cd Runtime && ./DEMO --scene-chase --deferred --chase-start=480"
echo "run it (lights):  cd Runtime && ./DEMO --scene-chase --deferred --cinematic --chase-start=1150"
echo "full chase:       cd Runtime && ./DEMO --scene-chase --deferred [--cinematic]"
