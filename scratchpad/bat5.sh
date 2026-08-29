#!/bin/zsh
set -u
ROUNDS=$1; OUT=$2; BIN=$3; shift 3
ARMS=("$@")
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd "$(dirname $BIN)" || exit 1
: > "$OUT"
BASE="--bench=scene@scene=city,t=1961,iters=20 --env_live_water --deferred --city_env_pixel --profiler=0 --deferred_prof=1"
RX='^\[DPROF\] renderFrame |^\[DPROF\]   fastfog|^\[DPROF\]     fog-composite|^\[DPROF\]     fog-columns|^\[DPROF\]   cones-call'
for r in $(seq 1 $ROUNDS); do
  for a in $ARMS; do
    lbl=${a%%:*}; xf=${a#*:}; [ "$xf" = "$lbl" ] && xf=""
    for mode in wall hw; do
      hp=""; [ $mode = hw ] && hp="--hw_prof"
      line=$("$BIN" ${=BASE} ${=hp} ${=xf} 2>&1 | grep -E "$RX")
      echo "r=$r arm=$lbl mode=$mode $(echo $line | tr '\n' '~')" >> "$OUT"
    done
  done
done
