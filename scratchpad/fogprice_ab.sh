#!/bin/zsh
# 2026-08-16y — interleaved, order-ROTATED A/B/C over the city acceptance arm.
#   usage: fogprice_ab.sh <outdir> <rounds> <iters> <poses,csv> <arm1> [arm2 ...]
# An ARM is "label:binary" or "label:binary:extra flags". Every arm runs once per
# round, and the arm ORDER rotates by round so no arm is permanently first
# (thermal/scheduler drift is the reason the campaign requires this).
set -u
WT=/Users/gil-ad/work/rev-fogprice
OUT=${1:?outdir}; ROUNDS=${2:-12}; ITERS=${3:-30}; POSES=(${(s:,:)4:-1961})
shift 4
ARMS=($@)
BASE=(--env_live_water --deferred --city_env_pixel)
mkdir -p $OUT
print -r -- "# fogprice_ab rounds=$ROUNDS iters=$ITERS poses=$POSES arms=$ARMS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 0 $((ROUNDS-1))); do
  n=${#ARMS[@]}
  for k in $(seq 0 $((n-1))); do
    idx=$(( (k + r) % n + 1 ))
    spec=${ARMS[$idx]}
    lbl=${spec%%:*}; rest=${spec#*:}
    bin=${rest%%:*}; extra=""
    [[ $rest == *:* ]] && extra=${rest#*:}
    for pose in $POSES; do
      (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$bin \
        --bench=scene@scene=city,t=$pose,iters=$ITERS,xres=1512,yres=848 \
        $BASE ${=extra} --profiler=1 --deferred_prof=5 --hw_prof 2>&1) \
        > $OUT/${lbl}_t${pose}_r${r}.txt
    done
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done
