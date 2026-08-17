#!/usr/bin/env bash
# reflmir_shots2.sh — before/after frame battery for --refl_correct, on the
# rebased (cb6aad4c) parent/child pair. ONE sweep per binary per scene, so the
# frames correspond to the pinned ones.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT=${OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/reflmir2}
mkdir -p "$OUT/before" "$OUT/after"

CHASE_T=100,400,800,1000,1300,1600
./DEMO_par --snapshot=chase@t=$CHASE_T --out="$OUT/before" --deferred >/dev/null 2>&1
./DEMO_new --snapshot=chase@t=$CHASE_T --out="$OUT/after"  --deferred >/dev/null 2>&1

# city — the user's acceptance arm, one pose per process (a multi-pose sweep
# has its own temporal history and hashes differently).
for t in 1961 2400; do
  ./DEMO_par --snapshot=city@t=$t --out="$OUT/before" --env_live_water --deferred --city_env_pixel >/dev/null 2>&1
  ./DEMO_new --snapshot=city@t=$t --out="$OUT/after"  --env_live_water --deferred --city_env_pixel >/dev/null 2>&1
done
ls -1 "$OUT/before" "$OUT/after"
