#!/usr/bin/env bash
# 2026-08-16r gate battery: 11 pin recipes + the greets acceptance poses,
# differential parent-vs-child in ONE worktree / ONE asset tree.
# usage: xform_pins.sh <binA> [binB ...]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
N=${N:-3}
hash_run() {  # $1=bin  $2=tag  rest=argv
  local b="$1" tag="$2"; shift 2
  local d; d=$(mktemp -d)
  ./"$b" "$@" --out="$d" >/dev/null 2>&1
  printf '%-10s %-34s %s\n' "$b" "$tag" "$(md5 -q "$d"/*.ppm 2>/dev/null | tr '\n' ' ')"
  rm -rf "$d"
}
for b in "$@"; do
 for i in $(seq 1 $N); do
  hash_run "$b" city-t1961-plain    --snapshot=city@t=1961    --deferred --profiler=0
  hash_run "$b" city-t1961-arm      --snapshot=city@t=1961    --env_live_water --deferred --city_env_pixel --profiler=0
  hash_run "$b" city-t2400-arm      --snapshot=city@t=2400    --env_live_water --deferred --city_env_pixel --profiler=0
  hash_run "$b" city-t400-arm       --snapshot=city@t=400     --env_live_water --deferred --city_env_pixel --profiler=0
  # chase: recorded recipe is VERBATIM without --profiler=0 (it is still
  # byte-relevant there; with it the five hashes are a different, self-consistent set)
  hash_run "$b" chase-5pose         --snapshot=chase@t=100,400,800,1200,1600 --deferred
  hash_run "$b" fountain-t2500      --snapshot=fountain@t=2500 --deferred --hdr --glass-refract=1 --glass-test --profiler=0
  hash_run "$b" greets-acc-t5743    --snapshot=greets@t=5743 --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0
  hash_run "$b" greets-acc-t2845    --snapshot=greets@t=2845 --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0
  hash_run "$b" greets-acc-t6097    --snapshot=greets@t=6097 --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0
  hash_run "$b" greets-acc-t6133    --snapshot=greets@t=6133 --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0
 done
done
