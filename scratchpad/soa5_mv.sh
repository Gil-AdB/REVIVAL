#!/usr/bin/env bash
# MAIN-VIEW half of the Phase 5 pricing ladder. --xfrm_par=0 so --xfrm_prof's
# per-bucket VERT/FACE/TOTAL numbers exist at all (the sharded driver early-
# returns before the accounting). Per-frame MIN over ITERS frames, R rounds,
# order rotated.
#   soa5_mv.sh <rounds> <scene> <t> "<bin:ablate ...>" [-- extra flags]
set -u
R="$1"; SCENE="$2"; T="$3"; ARMS=($4); shift 4; [ "${1:-}" = "--" ] && shift
EXTRA=("$@")
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ITERS=${ITERS:-24}
XR=${XR:-1920}; YR=${YR:-1080}
run() {
  local bin="${1%%:*}" ab="${1#*:}"
  ./"$bin" --bench=scene@scene="$SCENE",t="$T",iters="$ITERS",xres="$XR",yres="$YR" \
    "${EXTRA[@]}" --xfrm_par=0 --xfrm_prof="$ITERS" --xfrm_ablate="$ab" --profiler=0 2>&1 | awk '
    /XFRM-PROF/ {
      gotT=0; gotV=0; gotF=0
      for (i=1;i<=NF;i++) {
        if ($i=="TOTAL" && !gotT) { gotT=1; tot=$(i+1) }
        if ($i=="VERT"  && !gotV) { gotV=1; vert=$(i+1) }
        if ($i=="FACE"  && !gotF) { gotF=1; face=$(i+1) }
      }
    }
    /frame_ms min/ { split($0,a,"= "); split(a[2],b,"/"); fmin=b[1] }
    END { printf "%s,%s,%s,%s\n", tot, vert, face, fmin }'
}
for a in "${ARMS[@]}"; do run "$a" > /dev/null; done
N=${#ARMS[@]}
for ((r=0;r<R;r++)); do
  for ((k=0;k<N;k++)); do
    idx=$(( (k + r) % N )); echo "$r,${ARMS[$idx]},$(run "${ARMS[$idx]}")"
  done
done
