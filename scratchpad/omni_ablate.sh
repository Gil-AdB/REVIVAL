#!/bin/zsh
# Omni-loop ablation ladder (FDS_OMNI_ABLATE, DeferredSurfaceKernel.cpp).
# For each stage n: rewrite the #define, rebuild that TU + relink, run the
# greets benches, keep the raw DPROF. Instruction counts are deterministic for
# a fixed binary, so RUNS=3 with run 1 discarded is ample; the two-session
# requirement is met by running this script twice into different OUT dirs.
#
#   usage: omni_ablate.sh [runs] [iters] [outdir] [stages...]
set -u
WT=/Users/gil-ad/work/rev-omniloop
SRC=$WT/FDS/RENDER/DeferredSurfaceKernel.cpp
RUNS=${1:-3}; ITERS=${2:-12}; OUT=${3:-/tmp/omniloop/ladderA}
shift 3 2>/dev/null || shift $#
if (( $# )); then STAGES=($@); else STAGES=(0 1 2 3 4 5 6 7 8 9 10 11); fi
HIS_CAM="-8.6249094,2.72651696,-53.2339516,0.210607708,0.0055912463,-0.977554619"
mkdir -p $OUT

print -r -- "# omni ablation ladder runs=$RUNS iters=$ITERS out=$OUT"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"

for st in $STAGES; do
  perl -pi -e "s/^#define FDS_OMNI_ABLATE .*\$/#define FDS_OMNI_ABLATE $st/" $SRC
  grep -q "^#define FDS_OMNI_ABLATE $st\$" $SRC || { print -r -- "!! sed failed stage $st"; exit 2; }
  cmake --build $WT/build >/dev/null 2>&1 || { print -r -- "!! build failed stage $st"; exit 2; }
  cp $WT/build/DEMO/DEMO $WT/Runtime/DEMO
  rm -f $OUT/s${st}_*.txt
  for r in $(seq 1 $RUNS); do
    (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO \
      --bench=scene@scene=greets,t=5743,iters=$ITERS,xres=1920,yres=1080 \
      --deferred_prof=1 --hw_prof --strict_flags 2>&1) \
      | grep -E '^\[DPROF\]' > $OUT/s${st}_t5743_r${r}.txt
    (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      FDS_GREETS_CAM=$HIS_CAM ./DEMO \
      --bench=scene@scene=greets,t=3122,iters=$ITERS,xres=1512,yres=848 \
      --deferred_prof=1 --hw_prof --strict_flags 2>&1) \
      | grep -E '^\[DPROF\]' > $OUT/s${st}_his_r${r}.txt
  done
  print -r -- "# stage $st done (load $(uptime | sed 's/.*averages: //'))"
done

perl -pi -e "s/^#define FDS_OMNI_ABLATE \\d+\$/#define FDS_OMNI_ABLATE 0/" $SRC
print -r -- "# restored FDS_OMNI_ABLATE 0"
