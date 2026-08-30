#!/bin/zsh
# v4 phase-1 gate: render greets cam A t=5965 on the judging flags with the
# phase-1 census on, then read the census with tools/v4_census.py.
#
#   tools/v4_p1_gate.sh            # human table + the eight P1 checks; exit 1 on FAIL
#   tools/v4_p1_gate.sh --json     # one JSON object (what the ledger rechecks parse)
#   tools/v4_p1_gate.sh --log      # print the raw [V4-*] block instead
#
# Never opens a window (SDL dummy drivers) and never keeps a raw plane: the
# snapshot's PPM is the only output and it is deleted on the way out.  Run from
# anywhere; paths are resolved from this script.
set -u

MODE="gate"
[[ "${1:-}" == "--json" ]] && MODE="json"
[[ "${1:-}" == "--log"  ]] && MODE="log"

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
RT="$ROOT/Runtime"
OUT="${TMPDIR:-/tmp}/v4_p1_gate"
CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"

mkdir -p "$OUT"
( cd "$RT" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="$CAM" \
    ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
           --greets-displace --greets_displace_v4 --v4_census \
           --force_xres=1920 --force_yres=1080 \
           --snapshot=greets@t=5965 --out="$OUT" ) > "$OUT/log.txt" 2>&1
rc=$?
if (( rc != 0 )); then
  echo "v4_p1_gate: DEMO exited $rc — see $OUT/log.txt" >&2
  exit 2
fi
rm -f "$OUT"/*.ppm

case "$MODE" in
  log)  grep -E '^\[V4-' "$OUT/log.txt" ;;
  json) python3 "$HERE/v4_census.py" --json "$OUT/log.txt" ;;
  *)    python3 "$HERE/v4_census.py" --gate --junctions 8 "$OUT/log.txt" ;;
esac
