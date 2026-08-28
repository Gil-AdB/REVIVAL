#!/bin/zsh
# Interleaved battery over arms given as label:BINPATH:extraflags
set -u
ROUNDS=$1; OUT=$2; SCENE=$3; T=$4; shift 4
ARMS=("$@")
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
: > "$OUT"
BASE="--bench=scene@scene=$SCENE,t=$T,iters=20 --profiler=0 --deferred_prof=1"
EXTRA=""
[ "$SCENE" = city ] && EXTRA="--env_live_water --deferred --city_env_pixel"
[ "$SCENE" = chase ] && EXTRA="--deferred"
[ "$SCENE" = greets ] && EXTRA="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace"
for r in $(seq 1 $ROUNDS); do
  for a in $ARMS; do
    lbl=${a%%:*}; rest=${a#*:}; bin=${rest%%:*}; xf=${rest#*:}; [ "$xf" = "$bin" ] && xf=""
    for mode in wall hw; do
      hp=""; [ $mode = hw ] && hp="--hw_prof"
      line=$(cd "$(dirname $bin)" && "$bin" ${=BASE} ${=EXTRA} ${=hp} ${=xf} 2>&1 | grep -E "^\[DPROF\]   cones-call|^\[DPROF\] renderFrame ")
      echo "r=$r arm=$lbl mode=$mode $(echo $line | tr '\n' '~')" >> "$OUT"
    done
  done
done
