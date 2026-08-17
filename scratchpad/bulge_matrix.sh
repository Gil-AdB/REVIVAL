#!/bin/zsh
# Pairwise flag matrix at t=5970: singles + pairs of {F,B,W,P,M}, junction census on.
# Metric per arm from the bake log via xsec.py.
cd "$(dirname "$0")/../Runtime" || exit 1
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
CAM="18.8969765,3.21025538,-58.888485,-0.896694958,-0.0735020638,0.436503887"
OUT=/tmp/xsec
mkdir -p "$OUT"

# arm spec: name:flags-to-DISABLE (all five default on under the umbrella)
run_arm() {
  local name=$1; shift
  local d="$OUT/$name"
  mkdir -p "$d"
  FDS_GREETS_CAM="$CAM" ./DEMO --snapshot=greets@t=5970 --out=$d \
    --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
    --greets-displace --greets_displace_junction_census --profiler=0 \
    "$@" > "$d/log.txt" 2>&1
  printf "%-8s rc=%d " "$name" $?
  python3 ../scratchpad/xsec.py "$d/log.txt" brief | tail -1
}

NOF=--no-greets_displace_free_edge
NOB=--greets_displace_border_mean=0
NOW=--no-greets_displace_seam_weld
NOP=--no-greets_displace_plane_normal
NOM=--no-greets_displace_mitre

run_arm alloff  $NOF $NOB $NOW $NOP $NOM
# singles: only X on
run_arm onlyF   $NOB $NOW $NOP $NOM
run_arm onlyB   $NOF $NOW $NOP $NOM
run_arm onlyW   $NOF $NOB $NOP $NOM
run_arm onlyP   $NOF $NOB $NOW $NOM
run_arm onlyM   $NOF $NOB $NOW $NOP
# pairs: only X+Y on
run_arm FB      $NOW $NOP $NOM
run_arm FW      $NOB $NOP $NOM
run_arm FP      $NOB $NOW $NOM
run_arm FM      $NOB $NOW $NOP
run_arm BW      $NOF $NOP $NOM
run_arm BP      $NOF $NOW $NOM
run_arm BM      $NOF $NOW $NOP
run_arm WP      $NOF $NOB $NOM
run_arm WM      $NOF $NOB $NOP
run_arm PM      $NOF $NOB $NOW
echo MATRIX-DONE
