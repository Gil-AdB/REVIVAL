#!/bin/zsh
# Interleaved, order-rotated A/B for the wave-2 oct-pair round (2026-08-16o).
#   usage: w2oct_ab.sh <rounds> <iters> <outdir> <pose,pose,...> [arm,arm,...]
# Arms (see /tmp/w2oct/DEMO_*):
#   par    parent 8ea787f9
#   h4off  hatched mode-4 child, --no-deferred_fill_oct_pair  (prices the
#          restructure by itself, exactly as 16g/16h/16i did)
#   h4on   hatched mode-4 child, flag ON
#   f4     FLAGLESS mode 4 (neighbours + centre in one 4-wide decode)
#   f2     FLAGLESS mode 2 (the two neighbours only — 16h's named candidate)
set -u
WT=/Users/gil-ad/work/rev-w2oct
ROUNDS=${1:-11}; ITERS=${2:-10}; OUT=${3:-/tmp/w2oct/ab1}; POSES=(${(s:,:)4:-5743})
ARMSEL=(${(s:,:)5:-par,h4off,h4on,f4,f2})
ARM=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
typeset -A BINOF FLAGOF
BINOF=(par /tmp/w2oct/DEMO_parent  h4off /tmp/w2oct/DEMO_h4  h4on /tmp/w2oct/DEMO_h4 \
       f4 /tmp/w2oct/DEMO_f4       f2 /tmp/w2oct/DEMO_f2)
FLAGOF=(par ""  h4off "--no-deferred_fill_oct_pair"  h4on ""  f4 ""  f2 "")
mkdir -p $OUT
print -r -- "# w2oct A/B rounds=$ROUNDS iters=$ITERS poses=$POSES arms=$ARMSEL load: $(uptime | sed 's/.*averages: //')"
N=${#ARMSEL}
for r in $(seq 1 $ROUNDS); do
  for k in $(seq 1 $N); do
    idx=$(( (k + r - 2) % N + 1 ))
    nm=${ARMSEL[$idx]}; bn=${BINOF[$nm]}; fl=${FLAGOF[$nm]}
    cp $bn $WT/Runtime/DEMO
    for pose in $POSES; do
      (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO \
        --bench=scene@scene=greets,t=$pose,iters=$ITERS,xres=1512,yres=848 \
        $ARM ${=fl} --deferred_prof=1 --hw_prof --profiler=0 --strict_flags 2>&1) \
        | grep -E '^\[DPROF\]' > $OUT/${nm}_t${pose}_r${r}.txt
    done
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done
