#!/bin/zsh
set -u
ROUNDS=$1; OUT=$2; shift 2
ARMS=("$@")
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
: > "$OUT"
BASE="--bench=scene@scene=city,t=1961,iters=20 --env_live_water --deferred --city_env_pixel --profiler=0 --deferred_prof=1"
RX='^\[DPROF\] renderFrame |^\[DPROF\]   fastfog|^\[DPROF\]     fog-composite|^\[DPROF\]     fog-columns|^\[DPROF\]   cones-call'
for r in $(seq 1 $ROUNDS); do
  for a in $ARMS; do
    lbl=${a%%:*}; bin=${a#*:}
    for mode in wall hw; do
      hp=""; [ $mode = hw ] && hp="--hw_prof"
      line=$(cd "$(dirname $bin)" && "$bin" ${=BASE} ${=hp} 2>&1 | grep -E "$RX")
      echo "r=$r arm=$lbl mode=$mode $(echo $line | tr '\n' '~')" >> "$OUT"
    done
  done
done
