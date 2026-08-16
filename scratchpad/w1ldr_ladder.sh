#!/bin/zsh
# WAVE-1 LDR-chain ablation on Gil-Ad's GREETS ACCEPTANCE ARM.
# Stage 0 must produce a binary byte-identical to the parent (it does).
#   usage: w1ldr_ladder.sh [runs] [iters] [outdir] [poses] [stages...]
set -u
WT=/Users/gil-ad/work/rev-w1ldr
SRC=$WT/FDS/RENDER/DeferredSurfaceKernel.cpp
RUNS=${1:-4}; ITERS=${2:-10}; OUT=${3:-/tmp/w1/ladder}; POSES=(${(s:,:)4:-5743})
shift 4 2>/dev/null || shift $#
if (( $# )); then STAGES=($@); else STAGES=(0 1 2); fi
ARM=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
mkdir -p $OUT
print -r -- "# w1ldr ladder runs=$RUNS iters=$ITERS poses=$POSES out=$OUT load: $(uptime | sed 's/.*averages: //')"
for st in $STAGES; do
  perl -pi -e "s/^#define FDS_W1LDR_ABLATE .*\$/#define FDS_W1LDR_ABLATE $st/" $SRC
  grep -q "^#define FDS_W1LDR_ABLATE $st\$" $SRC || { print -r -- "!! sed failed stage $st"; exit 2; }
  cmake --build $WT/build >/dev/null 2>&1 || { print -r -- "!! build failed stage $st"; exit 2; }
  cp $WT/build/DEMO/DEMO $WT/Runtime/DEMO
  rm -f $OUT/s${st}_*.txt
  for r in $(seq 1 $RUNS); do
    for pose in $POSES; do
      (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO \
        --bench=scene@scene=greets,t=$pose,iters=$ITERS,xres=1512,yres=848 \
        $ARM --deferred_prof=1 --hw_prof --profiler=0 --strict_flags 2>&1) \
        | grep -E '^\[DPROF\]' > $OUT/s${st}_t${pose}_r${r}.txt
    done
  done
  print -r -- "# stage $st done (load $(uptime | sed 's/.*averages: //'))"
done
perl -pi -e "s/^#define FDS_W1LDR_ABLATE \\d+\$/#define FDS_W1LDR_ABLATE 0/" $SRC
cmake --build $WT/build >/dev/null 2>&1
print -r -- "# restored FDS_W1LDR_ABLATE 0"
