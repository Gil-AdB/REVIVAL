#!/usr/bin/env bash
# 11-pin gate battery (16f/16r set) + the four greets ACCEPTANCE poses.
# Differential: run with two binaries in ONE worktree / ONE asset tree.
#   soa5_pins.sh <binA> [binB ...]        N=3 by default
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
N=${N:-3}
hash_run() {
  local b="$1" tag="$2"; shift 2
  local d; d=$(mktemp -d)
  ./"$b" "$@" --out="$d" >/dev/null 2>&1
  printf '%-10s %-24s %s\n' "$b" "$tag" "$(md5 -q "$d"/*.ppm 2>/dev/null | tr '\n' ' ')"
  rm -rf "$d"
}
GCAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"
ACC="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0"
for b in "$@"; do
 for i in $(seq 1 "$N"); do
  FDS_CITY_ENV_PIXEL=1 hash_run "$b" city-t1961-plain --snapshot=city@t=1961 --deferred --profiler=0
  hash_run "$b" city-t1961-arm   --snapshot=city@t=1961 --env_live_water --deferred --city_env_pixel --profiler=0
  hash_run "$b" city-t2400-arm   --snapshot=city@t=2400 --env_live_water --deferred --city_env_pixel --profiler=0
  hash_run "$b" city-t400-arm    --snapshot=city@t=400  --env_live_water --deferred --city_env_pixel --profiler=0
  # chase: recorded recipe is VERBATIM, WITHOUT --profiler=0 (16r trap)
  hash_run "$b" chase-5pose      --snapshot=chase@t=100,400,800,1200,1600 --deferred
  hash_run "$b" fountain-t2500   --snapshot=fountain@t=2500 --deferred --hdr --glass-refract=1 --glass-test --profiler=0
  FDS_GREETS_CAM="$GCAM" hash_run "$b" greets-t1588-pin \
      --snapshot=greets@t=1588 --deferred --hdr --glass-refract=1 --glass-test \
      --xpar-peel-passes=4 --profiler=0 --no-env_refl
  # shellcheck disable=SC2086
  hash_run "$b" greets-acc-t5743 --snapshot=greets@t=5743 $ACC
  hash_run "$b" greets-acc-t2845 --snapshot=greets@t=2845 $ACC
  hash_run "$b" greets-acc-t6097 --snapshot=greets@t=6097 $ACC
  hash_run "$b" greets-acc-t6133 --snapshot=greets@t=6133 $ACC
 done
done
