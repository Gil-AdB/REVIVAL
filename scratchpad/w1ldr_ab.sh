#!/bin/zsh
# Interleaved, order-rotated three-arm A/B for the wave-1 LDR skip.
# par = the parent binary; off/on = the CHILD with the flag off/on, so the OFF
# column prices the restructure by itself.
#   usage: w1ldr_ab.sh <rounds> <iters> <outdir> <pose,pose,...>
set -u
WT=/Users/gil-ad/work/rev-w1ldr
ROUNDS=${1:-11}; ITERS=${2:-10}; OUT=${3:-/tmp/w1/ab1}; POSES=(${(s:,:)4:-5743})
ARM=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
typeset -a NAMES BINS FLAGS
NAMES=(par off on)
BINS=(DEMO_par DEMO_chi DEMO_chi)
FLAGS=(
  ""
  "--no-deferred_shade_ldr_skip"
  ""
)
mkdir -p $OUT
print -r -- "# w1ldr A/B rounds=$ROUNDS iters=$ITERS poses=$POSES load: $(uptime | sed 's/.*averages: //')"
N=${#NAMES}
for r in $(seq 1 $ROUNDS); do
  for k in $(seq 1 $N); do
    idx=$(( (k + r - 2) % N + 1 ))
    nm=${NAMES[$idx]}; bn=${BINS[$idx]}; fl=${FLAGS[$idx]}
    for pose in $POSES; do
      (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$bn \
        --bench=scene@scene=greets,t=$pose,iters=$ITERS,xres=1512,yres=848 \
        $ARM ${=fl} --deferred_prof=1 --hw_prof --profiler=0 --strict_flags 2>&1) \
        | grep -E '^\[DPROF\]' > $OUT/${nm}_t${pose}_r${r}.txt
    done
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done
