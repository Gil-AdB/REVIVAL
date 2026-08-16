#!/usr/bin/env bash
# 2026-08-16r ladder. The change lives in the OFFSCREEN passes (mirror RTT +
# env/SH probes), which sit inside renderFrame's RTT row — so unlike the
# shadow-bake rounds, renderFrame IS the column that carries the result.
# Columns: frame min, TOTL, renderFrame wall/Ginstr/Gcyc, gbuffer Ginstr, RNDR min,
# XFRM min, ANIM min. The mirror RTT bakes live inside RNDR and OUTSIDE
# renderFrame, so renderFrame's Ginstr column is this round's INERTNESS control
# and RNDR/TOTL carry the result.
#   xform_ladder.sh <rounds> <scene> <t> "<arm1 arm2 ...>" -- <flags...>
# arm = "<binary>[:extra flags]"
set -u
R="$1"; SCENE="$2"; T="$3"; ARMS=($4); shift 4; [ "${1:-}" = "--" ] && shift
FLAGS=("$@")
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ITERS=${ITERS:-24}
WARM=${WARM:-4}
run() {
  local arm="$1" bin extra
  bin="${arm%%:*}"
  extra="${arm#*:}"; [ "$extra" = "$arm" ] && extra=""
  # shellcheck disable=SC2086
  ./"$bin" --bench=scene@scene="$SCENE",t="$T",iters="$ITERS",xres=1512,yres=848 \
     "${FLAGS[@]}" $extra --profiler=1 --deferred_prof="$WARM" --hw_prof 2>&1 | awk '
    /frame_ms min\/p50/ { split($0,a,"= "); split(a[2],b,"/"); fmin=b[1] }
    /^TOTL/             { totl=$2 }
    /^\[DPROF\] renderFrame/ { rfw=$4; rfi=$9; rfc=$10 }
    /^\[DPROF\]   gbuffer/   { gbi=$9 }
    /^RNDR/             { rndr=$4 }
    /^XFRM/             { xfrm=$4 }
    /^ANIM/             { anim=$4 }
    END { printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", fmin, totl, rfw, rfi, rfc, gbi, rndr, xfrm, anim }'
}
N=${#ARMS[@]}
for ((r=0; r<R; r++)); do
  for ((k=0; k<N; k++)); do
    idx=$(( (k + r) % N ))
    a="${ARMS[$idx]}"
    echo "$r,$a,$(run "$a")"
  done
done
