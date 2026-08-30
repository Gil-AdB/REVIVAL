#!/usr/bin/env bash
# One number out of the v4 phase-2 census, for a ledger recheck.
#   tools/v4_p2_num.sh <key> [extra DEMO flags...]
# <key> is any key of tools/v4_census.py --p2 (stone_faces, tvertices,
# under1deg, minang_p10, out_use3plus, border_max_dev, ...).  Renders greets
# cam A once with --greets_displace_v4 --v4_census and prints the value.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY="$1"; shift
OUT="${TMPDIR:-/tmp}/v4_p2_num"
rm -rf "$OUT"; mkdir -p "$OUT"
( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
   FDS_GREETS_CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \
   ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
          --greets-displace --force_xres=1920 --force_yres=1080 \
          --greets_displace_v4 --v4_census "$@" \
          --snapshot=greets@t=5965 --out="$OUT" ) >"$OUT/log.txt" 2>&1
rm -f "$OUT"/*.ppm
python3 "$ROOT/tools/v4_census.py" --p2 "$OUT/log.txt" | awk -v k="$KEY" '$1==k{print $2}'
