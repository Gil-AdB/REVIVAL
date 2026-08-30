#!/usr/bin/env bash
# One greets snapshot md5 on the judging flags, at an arbitrary pose/time and
# with arbitrary extra flags.  Never opens a window; never keeps the raw PPM.
#
#   tools/v4_md5.sh <tag> <t> <cam6> [extra flags...]
#
# Prints "<tag> <md5>".  The judging arm is the greets acceptance recipe the
# ledger's camA pin uses (--deferred --hdr --hdr-linear --texture-filter=2
# --ssao --ssao-gtao --greets-displace) at 1920x1080.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="$1"; T="$2"; CAM="$3"; shift 3
OUT="${TMPDIR:-/tmp}/v4_md5_$TAG.$$"
rm -rf "$OUT"; mkdir -p "$OUT"
( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="$CAM" \
   ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
          --greets-displace --force_xres=1920 --force_yres=1080 \
          "$@" --snapshot="greets@t=$T" --out="$OUT" ) >"$OUT/log.txt" 2>&1
rc=$?
if (( rc != 0 )); then echo "$TAG DEMO-EXIT-$rc ($OUT/log.txt)"; exit 2; fi
M=$(md5 -q "$OUT"/*_color.ppm 2>/dev/null)
echo "$TAG ${M:-NO-PPM}"
rm -rf "$OUT"
