#!/usr/bin/env bash
# Symbol SELF-time via macOS `sample`, for bench-capable scenes AND chase
# (which has no --bench arm: repeat the same snapshot timestamp N times).
# usage: selftime2.sh <bin> <scene> <t> <secs> -- <flags...>
set -u
BIN="$1"; SCENE="$2"; T="$3"; SECS="$4"; shift 4; [ "${1:-}" = "--" ] && shift
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT="${SELF_OUT:-${TMPDIR:-/tmp}/selftime2_${BIN}_${SCENE}_${T}.txt}"
SNAPDIR="${TMPDIR:-/tmp}/selfsnap_$$"; mkdir -p "$SNAPDIR"
if [ "$SCENE" = "chase" ]; then
  TS="$T"; for i in $(seq 1 200); do TS="$TS,$T"; done
  ./"$BIN" --snapshot=chase@t="$TS" --out="$SNAPDIR" "$@" --profiler=0 >/dev/null 2>&1 &
else
  ./"$BIN" --bench=scene@scene="$SCENE",t="$T",iters=400,xres=1512,yres=848 \
     "$@" --profiler=0 >/dev/null 2>&1 &
fi
PID=$!
sleep "${WARMSEC:-8}"
sample "$PID" "$SECS" -f "$OUT" >/dev/null 2>&1
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
rm -rf "$SNAPDIR"
echo "$OUT"
