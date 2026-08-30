#!/usr/bin/env bash
# Render ONE greets pose on the judging flags with the colour + z16 + G-buffer
# material planes, into $OUTDIR.  Never opens a window.
#   tools/v4_p2_pose.sh <outdir> <t> <cam6> [extra flags...]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$1"; T="$2"; CAM="$3"; shift 3
rm -rf "$OUT"; mkdir -p "$OUT"
( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
   FDS_GREETS_CAM="$CAM" FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 \
   ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
          --greets-displace --force_xres=1920 --force_yres=1080 \
          "$@" --snapshot="greets@t=$T" --out="$OUT" ) >"$OUT/log.txt" 2>&1
echo $?
