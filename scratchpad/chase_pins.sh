#!/bin/zsh
# Differential pin battery for the round-6 cone change.
# Runs the SAME recipes on two binaries in ONE tree, so the comparison is a
# DIFFERENTIAL one (SESSION_STATE's 2026-08-09c hazard: an absolute pin taken in
# a tree with concurrent edits is meaningless; two binaries, one tree, is not).
# Run 1 of each binary is discarded — the first run after a rebuild can write a
# cache the later runs read.
set -u
WT=/Users/gil-ad/work/rev-chasecone
OUT=${PIN_OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/pins}
RUNS=${1:-3}
BINS=(${2:-DEMO_base} ${3:-DEMO_new})
mkdir -p $OUT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"

run_one () {   # $1 bin  $2 tag  $3 run
  local b=$1 tag=$2 r=$3 d=$OUT/${tag}_${b}_r${r}
  rm -rf $d; mkdir -p $d
  case $tag in
    chase)  (cd $WT/Runtime && ./$b --snapshot=chase@t=100,400,800,1200,1600 --out=$d --deferred >/dev/null 2>&1) ;;
    greets) (cd $WT/Runtime && FDS_GREETS_CAM=$GREETS_CAM ./$b --snapshot=greets@t=1588 --out=$d \
              --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 \
              --profiler=0 --no-env_refl >/dev/null 2>&1) ;;
    city)   (cd $WT/Runtime && FDS_CITY_ENV_PIXEL=1 ./$b --snapshot=city@t=1961 --out=$d --deferred >/dev/null 2>&1) ;;
    fountain) (cd $WT/Runtime && ./$b --snapshot=fountain@t=2500 --out=$d --deferred --hdr \
              --glass-refract=1 --glass-test --profiler=0 >/dev/null 2>&1) ;;
  esac
  for f in $d/*.ppm(N); do print -r -- "$(md5 -q $f)  $(basename $f)"; done
}

for tag in chase greets city fountain; do
  print -r -- "===== $tag ====="
  for b in $BINS; do
    for r in $(seq 1 $RUNS); do
      out=$(run_one $b $tag $r)
      (( r == 1 )) && continue            # discard run 1
      print -r -- "-- $b run$r"
      print -r -- "$out"
    done
  done
done
