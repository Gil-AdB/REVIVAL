#!/usr/bin/env bash
# A/B snapshot pair for the --v4_world_uv arm at one pose, into docs/img/worlduv.
# Control = the SAME v4 pipeline WITHOUT --v4_world_uv (the per-polygon-mip trap
# ba7c2e825b1f: the control may never be --no-greets_displace), so the only
# difference between the two frames is the UV rewrite.
#
#   tools/v4_worlduv_ab.sh <tag> <t> <cam6> [extra flags shared by both arms...]
#
# Writes <tag>_before.png / <tag>_after.png / <tag>_diff.png and prints the
# snapshot_diff summary line.  Never opens a window.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="$1"; T="$2"; CAM="$3"; shift 3
OUT="${TMPDIR:-/tmp}/wuvab_$TAG.$$"
IMG="$ROOT/docs/img/worlduv"
rm -rf "$OUT"; mkdir -p "$OUT/a" "$OUT/b" "$IMG"
for arm in a b; do
  EX=(--greets_displace_v4)
  [ "$arm" = b ] && EX+=(--v4_world_uv)
  ( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
     FDS_GREETS_CAM="$CAM" ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 \
        --ssao --ssao-gtao --greets-displace --force_xres=1920 --force_yres=1080 \
        "${EX[@]}" "$@" --snapshot="greets@t=$T" --out="$OUT/$arm" ) >"$OUT/$arm/log.txt" 2>&1
done
A=$(ls "$OUT"/a/*_color.ppm 2>/dev/null | head -1)
B=$(ls "$OUT"/b/*_color.ppm 2>/dev/null | head -1)
if [ -z "$A" ] || [ -z "$B" ]; then echo "$TAG NO-PPM (see $OUT)"; exit 2; fi
python3 "$ROOT/tools/ppm2png.py" "$A" "$IMG/${TAG}_before.png" >/dev/null
python3 "$ROOT/tools/ppm2png.py" "$B" "$IMG/${TAG}_after.png"  >/dev/null
echo -n "$TAG "
python3 "$ROOT/tools/snapshot_diff.py" "$A" "$B" "$IMG/${TAG}_diff.png" | tr '\n' ' '
echo
rm -rf "$OUT"
