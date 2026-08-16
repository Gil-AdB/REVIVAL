#!/bin/zsh
# CUBE-TAP INTERIOR ablation ladder on Gil-Ad's GREETS ACCEPTANCE ARM, 1512x848.
# Sweeps -DFDS_CUBE_ABLATE inside CubeShadow_Sample (FDS/FILLERS/ShadowMap.h)
# with the omni loop pinned at -DFDS_OMNI_ABLATE=9, so the loop `continue`s
# immediately after the tap and the only thing downstream of the constant 1.0f
# the cuts return is the `cubeAtten <= 0.0f` early-out.
#   usage: cube_ladder.sh [runs] [iters] [outdir] [stages...]
# Stage "x" is the special reference build FDS_OMNI_ABLATE=8 (no tap at all).
set -u
WT=/Users/gil-ad/work/rev-cubetap
SRC=$WT/FDS/FILLERS/ShadowMap.h
KSRC=$WT/FDS/RENDER/DeferredSurfaceKernel.cpp
RUNS=${1:-3}; ITERS=${2:-10}; OUT=${3:-/tmp/cube/ladderA}
shift 3 2>/dev/null || shift $#
if (( $# )); then STAGES=($@); else STAGES=(x 1 2 3 4 5 6 7 8 9 10 11 12 0); fi
ARM=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
mkdir -p $OUT
print -r -- "# cube ladder runs=$RUNS iters=$ITERS out=$OUT load: $(uptime | sed 's/.*averages: //')"
for st in $STAGES; do
  if [[ $st == x ]]; then omni=8; cube=0; else omni=9; cube=$st; fi
  perl -pi -e "s/^#define FDS_CUBE_ABLATE .*\$/#define FDS_CUBE_ABLATE $cube/" $SRC
  perl -pi -e "s/^#define FDS_OMNI_ABLATE .*\$/#define FDS_OMNI_ABLATE $omni/" $KSRC
  grep -q "^#define FDS_CUBE_ABLATE $cube\$" $SRC || { print -r -- "!! sed failed cube $cube"; exit 2; }
  grep -q "^#define FDS_OMNI_ABLATE $omni\$" $KSRC || { print -r -- "!! sed failed omni $omni"; exit 2; }
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
perl -pi -e "s/^#define FDS_CUBE_ABLATE \\d+\$/#define FDS_CUBE_ABLATE 0/" $SRC
perl -pi -e "s/^#define FDS_OMNI_ABLATE \\d+\$/#define FDS_OMNI_ABLATE 0/" $KSRC
print -r -- "# restored both ladders to 0"
