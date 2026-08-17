#!/usr/bin/env bash
# reflmir_shots.sh — before/after battery for --refl_correct (2026-08-16y).
# ONE sweep per binary per scene, so the frames correspond to the pinned ones.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT=${OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/reflmir}
mkdir -p "$OUT/before" "$OUT/after"

CHASE_T=100,400,800,1000,1300,1600
./DEMO_base --snapshot=chase@t=$CHASE_T --out="$OUT/before" --deferred >/dev/null 2>&1
./DEMO_fix  --snapshot=chase@t=$CHASE_T --out="$OUT/after"  --deferred >/dev/null 2>&1

for t in 1961 2400; do
  ./DEMO_base --snapshot=city@t=$t --out="$OUT/before" --env_live_water --deferred --city_env_pixel >/dev/null 2>&1
  ./DEMO_fix  --snapshot=city@t=$t --out="$OUT/after"  --env_live_water --deferred --city_env_pixel >/dev/null 2>&1
done

ls -1 "$OUT/before"
