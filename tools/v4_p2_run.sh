#!/usr/bin/env bash
# One v4 P2 arm: render greets at a pose on the judging flags with --v4_census
# and print the [V4-LATTICE]/[V4-OUT] block plus the colour md5.
#   tools/v4_p2_run.sh <tag> <t> <cam6> [extra flags...]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="$1"; T="$2"; CAM="$3"; shift 3
OUT="${TMPDIR:-/tmp}/v4_p2_$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"
( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="$CAM" \
   ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
          --greets-displace --force_xres=1920 --force_yres=1080 \
          --greets_displace_v4 --v4_census "$@" \
          --snapshot="greets@t=$T" --out="$OUT" ) >"$OUT/log.txt" 2>&1
rc=$?
grep -E '^\[V4-(LATTICE|OUT)\]' "$OUT/log.txt"
if (( rc == 0 )); then echo "MD5 $TAG $(md5 -q "$OUT"/*_color.ppm 2>/dev/null)"; else echo "MD5 $TAG DEMO-EXIT-$rc"; fi
rm -f "$OUT"/*.ppm
