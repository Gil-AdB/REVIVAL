#!/bin/zsh
# Omni-loop ablation ladder re-run on Gil-Ad's GREETS ACCEPTANCE ARM, 1512x848.
#   usage: dfl_ladder.sh [runs] [iters] [outdir] [stages...]
set -u
WT=/Users/gil-ad/work/rev-deflight
SRC=$WT/FDS/RENDER/DeferredSurfaceKernel.cpp
RUNS=${1:-2}; ITERS=${2:-10}; OUT=${3:-/tmp/dfl/ladderA}
shift 3 2>/dev/null || shift $#
if (( $# )); then STAGES=($@); else STAGES=(0 1 2 3 4 5 6 7 8 9 10 11); fi
ARM=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
mkdir -p $OUT
print -r -- "# dfl ladder runs=$RUNS iters=$ITERS out=$OUT load: $(uptime | sed 's/.*averages: //')"
for st in $STAGES; do
  perl -pi -e "s/^#define FDS_OMNI_ABLATE .*\$/#define FDS_OMNI_ABLATE $st/" $SRC
  grep -q "^#define FDS_OMNI_ABLATE $st\$" $SRC || { print -r -- "!! sed failed stage $st"; exit 2; }
  cmake --build $WT/build >/dev/null 2>&1 || { print -r -- "!! build failed stage $st"; exit 2; }
  cp $WT/build/DEMO/DEMO $WT/Runtime/DEMO
  rm -f $OUT/s${st}_*.txt
  for r in $(seq 1 $RUNS); do
    for pose in 5743 2845; do
      (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO \
        --bench=scene@scene=greets,t=$pose,iters=$ITERS,xres=1512,yres=848 \
        $ARM --deferred_prof=1 --hw_prof --profiler=0 --strict_flags 2>&1) \
        | grep -E '^\[DPROF\]' > $OUT/s${st}_t${pose}_r${r}.txt
    done
  done
  print -r -- "# stage $st done (load $(uptime | sed 's/.*averages: //'))"
done
perl -pi -e "s/^#define FDS_OMNI_ABLATE \\d+\$/#define FDS_OMNI_ABLATE 0/" $SRC
print -r -- "# restored FDS_OMNI_ABLATE 0"
