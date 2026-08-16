#!/bin/zsh
# Five-pose greets snapshot battery under HIS acceptance arm (the pin recipe
# does NOT exercise it), plus the city water-reflection UNDERLAY control that
# --deferred_fill_ldr_skip's predicate exists for. One pose per process.
#   usage: w1ldr_poses.sh <binary-name-in-Runtime> tag
set -u
WT=/Users/gil-ad/work/rev-w1ldr
BIN=$1; TAG=$2
OUT=${POSE_OUT:-/tmp/w1/poses}/$TAG
rm -rf $OUT; mkdir -p $OUT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd $WT/Runtime
for t in 2845 3409 5743 5813 6097; do
  ./$BIN --snapshot=greets@t=$t --out=$OUT/g$t \
      --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
      --greets-displace --profiler=0 >/dev/null 2>&1
done
# The control: CITY.CPP:3823's underlay renders into the MAIN VPage with the
# tonemap SKIPPED and CITY.CPP:3848 reads it straight back. Checkerboard forced
# so wave 2 runs too. ldrDiscarded must be FALSE here or this hash moves.
./$BIN --snapshot=city@t=1961 --out=$OUT/cityunderlay \
    --env_live_water --deferred --hdr --city_env_pixel --deferred_checkerboard \
    --profiler=0 >/dev/null 2>&1
for f in $(find $OUT -name '*.ppm' -o -name '*.PPM' | sort); do
  print -r -- "  $(md5 -q $f)  ${f:t}"
done
