#!/bin/zsh
# Pin gate for the wave-1 LDR campaign. The nine 2026-08-16f pins, in THIS
# worktree's Runtime/ (DEMO chdirs to its own directory, so the binary is run
# UNDER ITS OWN NAME instead of being copied over Runtime/DEMO — that lets the
# parent and child batteries run without stomping each other).
#   usage: w1ldr_pins.sh <binary-name-in-Runtime> tag [runs]
set -u
WT=/Users/gil-ad/work/rev-w1ldr
BIN=$1; TAG=$2; RUNS=${3:-3}
OUT=${PIN_OUT:-/tmp/w1/pins}/$TAG
mkdir -p $OUT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd $WT/Runtime
GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"
for r in $(seq 1 $RUNS); do
  d=$OUT/r$r; rm -rf $d; mkdir -p $d
  FDS_GREETS_CAM=$GREETS_CAM ./$BIN --snapshot=greets@t=1588 --out=$d/greets \
      --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 \
      --profiler=0 --no-env_refl >/dev/null 2>&1
  ./$BIN --snapshot=fountain@t=2500 --out=$d/fount --deferred --hdr \
      --glass-refract=1 --glass-test --profiler=0 >/dev/null 2>&1
  FDS_CITY_ENV_PIXEL=1 ./$BIN --snapshot=city@t=1961 --out=$d/city --deferred >/dev/null 2>&1
  ./$BIN --snapshot=city@t=1961 --out=$d/cityarm --env_live_water --deferred --city_env_pixel >/dev/null 2>&1
  ./$BIN --snapshot=chase@t=100,400,800,1200,1600 --out=$d/chase --deferred >/dev/null 2>&1
  print -r -- "== $TAG run $r =="
  for f in $(find $d -name '*.ppm' -o -name '*.PPM' | sort); do
    print -r -- "  $(md5 -q $f)  ${f#$d/}"
  done
done
