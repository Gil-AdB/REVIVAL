#!/usr/bin/env bash
# SoA Phase 5 pricing ladder. Reports the DynOmnis / DynMeshes shadow-bake
# xform WALL ms and xformCore ms per arm, min over the printed windows of a
# run and min over R order-rotated rounds.
#   soa5_ladder.sh <rounds> <t> "<arm ...>"     arm = "<bin>:<ablate>"
set -u
R="$1"; T="$2"; ARMS=($3)
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ITERS=${ITERS:-24}
XR=${XR:-1920}; YR=${YR:-1080}
ACC="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace"
run() {
  local bin="${1%%:*}" ab="${1#*:}"
  FDS_SHADOW_PROF_INTERVAL=8 ./"$bin" \
    --bench=scene@scene=greets,t="$T",iters="$ITERS",xres="$XR",yres="$YR" \
    $ACC --shadow-prof --xfrm_ablate="$ab" --profiler=0 2>&1 | awk '
    /SHADOW-PROF/ {
      mode = ($5 == "DynOmnis:") ? "O" : "M"
      first = 0
      for (i=1;i<=NF;i++) {
        if ($i ~ /^xform=/ && first == 0) { first = 1; v=$i; sub("xform=","",v); sub("ms","",v); w[mode]= (w[mode]==""||v+0<w[mode]) ? v+0 : w[mode] }
        if ($i ~ /^xformCore=/) { v=$i; sub("xformCore=","",v); sub("ms","",v); c[mode]= (c[mode]==""||v+0<c[mode]) ? v+0 : c[mode] }
      }
    }
    /frame_ms min/ { split($0,a,"= "); split(a[2],b,"/"); fmin=b[1] }
    END { printf "%s,%s,%s,%s,%s\n", w["O"], c["O"], w["M"], c["M"], fmin }'
}
# discard round 0
for a in "${ARMS[@]}"; do run "$a" > /dev/null; done
N=${#ARMS[@]}
for ((r=0;r<R;r++)); do
  for ((k=0;k<N;k++)); do
    idx=$(( (k + r) % N ))
    echo "$r,${ARMS[$idx]},$(run "${ARMS[$idx]}")"
  done
done
