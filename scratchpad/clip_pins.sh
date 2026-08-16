#!/usr/bin/env bash
# Differential pin battery for the clipper-residue round: the SAME recipes on
# two binaries in ONE tree, so the comparison is differential (an absolute pin
# taken in a tree with uncommitted authoring files is meaningless — see
# SESSION_STATE 2026-08-09c). Run 1 of each binary is discarded: the first run
# after a rebuild can write a cache the later runs read.
#   clip_pins.sh <runs> <binA> <binB>
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${PIN_OUT:-${TMPDIR:-/tmp}/clip_pins}"
RUNS=${1:-3}; A=${2:-DEMO_base}; B=${3:-DEMO}
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
run_one() {  # $1 bin  $2 tag  $3 run
  local b=$1 tag=$2 r=$3 d="$OUT/${tag}_${b}_r${r}"
  rm -rf "$d"; mkdir -p "$d"
  ( cd "$WT/Runtime" || exit 2
    case $tag in
      city_def)   FDS_CITY_ENV_PIXEL=1 ./$b --snapshot=city@t=1961 --out="$d" --deferred --profiler=0 ;;
      city_his)   ./$b --snapshot=city@t=1961 --out="$d" --env_live_water --deferred --city_env_pixel --profiler=0 ;;
      city_his24) ./$b --snapshot=city@t=2400 --out="$d" --env_live_water --deferred --city_env_pixel --profiler=0 ;;
      city_his04) ./$b --snapshot=city@t=400  --out="$d" --env_live_water --deferred --city_env_pixel --profiler=0 ;;
      chase)      ./$b --snapshot=chase@t=100,400,800,1200,1600 --out="$d" --deferred --profiler=0 ;;
      fountain)   ./$b --snapshot=fountain@t=2500 --out="$d" --deferred --hdr --glass-refract=1 --glass-test --profiler=0 ;;
      greets)     ./$b --snapshot=greets@t=1588 --out="$d" --deferred --profiler=0 ;;
      greets_his) ./$b --snapshot=greets@t=5743 --out="$d" --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0 ;;
      greets_his2) ./$b --snapshot=greets@t=2845 --out="$d" --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0 ;;
      greets_his3) ./$b --snapshot=greets@t=6097 --out="$d" --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0 ;;
      greets_his4) ./$b --snapshot=greets@t=6133 --out="$d" --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0 ;;
    esac >/dev/null 2>&1 )
  ls "$d"/*.ppm >/dev/null 2>&1 || { echo "    (no output)"; return; }
  for f in "$d"/*.ppm; do echo "    $(md5 -q "$f")  $(basename "$f")"; done
}
for tag in city_def city_his city_his24 city_his04 chase fountain greets greets_his greets_his2 greets_his3 greets_his4; do
  echo "===== $tag ====="
  for b in "$A" "$B"; do
    for r in $(seq 1 "$RUNS"); do
      out=$(run_one "$b" "$tag" "$r")
      [ "$r" = "1" ] && continue
      echo "  -- $b run$r"; echo "$out"
    done
  done
done
