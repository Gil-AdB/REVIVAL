#!/bin/zsh
# Interleaved two-binary battery. arms given as label:BINPATH
set -u
ROUNDS=$1; OUT=$2; SCENE=$3; T=$4; shift 4
ARMS=("$@")
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
: > "$OUT"
BASE="--bench=scene@scene=$SCENE,t=$T,iters=20 --profiler=0 --deferred_prof=1"
EXTRA=""
[ "$SCENE" = city ]   && EXTRA="--env_live_water --deferred --city_env_pixel"
[ "$SCENE" = chase ]  && EXTRA="--deferred"
[ "$SCENE" = greets ] && EXTRA="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace"
RX='^\[DPROF\] renderFrame |^\[DPROF\]   fastfog|^\[DPROF\]   cones-call|^\[DPROF\]   DeferredLighting-call|^\[DPROF\]     lighting-w1|^\[DPROF\]     fog-columns|^\[DPROF\]     fog-composite|^\[DPROF\]   gbuffer'
for r in $(seq 1 $ROUNDS); do
  for a in $ARMS; do
    lbl=${a%%:*}; bin=${a#*:}
    for mode in wall hw; do
      hp=""; [ $mode = hw ] && hp="--hw_prof"
      line=$(cd "$(dirname $bin)" && "$bin" ${=BASE} ${=EXTRA} ${=hp} 2>&1 | grep -E "$RX")
      echo "r=$r arm=$lbl mode=$mode $(echo $line | tr '\n' '~')" >> "$OUT"
    done
  done
done
