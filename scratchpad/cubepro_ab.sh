#!/bin/zsh
# Three-arm A/B for --deferred_cube_prepass on Gil-Ad's GREETS ACCEPTANCE ARM.
#   par = the parent binary (aa60d0ce)
#   off = the CHILD binary with the flag off  -> prices the restructure alone
#   on  = the CHILD binary with the flag on
# Order-rotated, interleaved, one pose per process.
#   usage: cubepro_ab.sh [rounds] [iters] [outdir] [poses...]
set -u
WT=/Users/gil-ad/work/rev-cubetap
ROUNDS=${1:-11}; ITERS=${2:-10}; OUT=${3:-/tmp/cube/ab}
shift 3 2>/dev/null || shift $#
if (( $# )); then POSES=($@); else POSES=(5743 2845 6097 3409 5813); fi
ARM=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
mkdir -p $OUT
print -r -- "# cubepro A/B rounds=$ROUNDS iters=$ITERS load: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  case $(( r % 3 )) in
    0) ORDER=(par off on) ;;
    1) ORDER=(off on par) ;;
    2) ORDER=(on par off) ;;
  esac
  for arm in $ORDER; do
    case $arm in
      par) BIN=./DEMO_par; EXTRA=() ;;
      off) BIN=./DEMO_new; EXTRA=() ;;
      on)  BIN=./DEMO_new; EXTRA=(--deferred_cube_prepass) ;;
    esac
    for pose in $POSES; do
      (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $BIN \
        --bench=scene@scene=greets,t=$pose,iters=$ITERS,xres=1512,yres=848 \
        $ARM $EXTRA --deferred_prof=1 --hw_prof --profiler=0 --strict_flags 2>&1) \
        | grep -E '^\[DPROF\]' > $OUT/${arm}_t${pose}_r${r}.txt
    done
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done
