#!/usr/bin/env bash
# Order-ROTATED N-way ladder. One pose per process.
#   ladder.sh <rounds> <scene> <t> "<bin1 bin2 ...>" -- <flags...>
# CSV: round,bin,framemin,totl,rf_wall,gbuf_wall,gbuf_thr,rf_ginstr,gbuf_ginstr,rf_gcyc,gbuf_gcyc
set -u
R="$1"; SCENE="$2"; T="$3"; BINS=($4); shift 4; [ "${1:-}" = "--" ] && shift
FLAGS=("$@")
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ITERS=${ITERS:-14}
WARM=${WARM:-4}
SNAP="${TMPDIR:-/tmp}/ladder_snap_$$"; mkdir -p "$SNAP"
run() {
  local bin="$1"
  if [ "$SCENE" = "chase" ]; then
    local TS="$T"; for i in $(seq 1 11); do TS="$TS,$T"; done
    ./"$bin" --snapshot=chase@t="$TS" --out="$SNAP" "${FLAGS[@]}" \
       --profiler=1 --deferred_prof="$WARM" --hw_prof 2>&1
  else
    ./"$bin" --bench=scene@scene="$SCENE",t="$T",iters="$ITERS",xres=1512,yres=848 \
       "${FLAGS[@]}" --profiler=1 --deferred_prof="$WARM" --hw_prof 2>&1
  fi | awk '
    /frame_ms min\/p50/ { split($0,a,"= "); split(a[2],b,"/"); fmin=b[1] }
    /^TOTL/             { totl=$2 }
    /^\[DPROF\] renderFrame/ { rfw=$4; rfi=$9; rfc=$10 }
    /^\[DPROF\]   gbuffer/   { gbw=$4; gbt=$6; gbi=$9; gbc=$10 }
    END { printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", fmin, totl, rfw, gbw, gbt, rfi, gbi, rfc, gbc }'
}
N=${#BINS[@]}
for ((r=0; r<R; r++)); do
  for ((k=0; k<N; k++)); do
    idx=$(( (k + r) % N ))
    b="${BINS[$idx]}"
    echo "$r,$b,$(run "$b")"
  done
done
rm -rf "$SNAP"
