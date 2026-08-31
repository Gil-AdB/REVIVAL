#!/usr/bin/env bash
# UNDISPLACED vs DISPLACED, both under the new UV (--v4_world_uv): the arm's own
# relief, isolated from the UV change.  a = --v4_amp=0, b = the shipped amplitude.
#   tools/v4_worlduv_disp.sh <tag> <t> <cam6>
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="$1"; T="$2"; CAM="$3"; shift 3
OUT="${TMPDIR:-/tmp}/wuvdisp_$TAG.$$"; IMG="$ROOT/docs/img/worlduv"
rm -rf "$OUT"; mkdir -p "$OUT/a" "$OUT/b" "$IMG"
for arm in a b; do
  EX=(--greets_displace_v4 --v4_world_uv)
  [ "$arm" = a ] && EX+=(--v4_amp=0)
  ( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
     FDS_GREETS_CAM="$CAM" ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 \
        --ssao --ssao-gtao --greets-displace --force_xres=1920 --force_yres=1080 \
        "${EX[@]}" "$@" --snapshot="greets@t=$T" --out="$OUT/$arm" ) >"$OUT/$arm/log.txt" 2>&1
done
A=$(ls "$OUT"/a/*_color.ppm|head -1); B=$(ls "$OUT"/b/*_color.ppm|head -1)
python3 "$ROOT/tools/ppm2png.py" "$A" "$IMG/${TAG}_undisplaced.png" >/dev/null
python3 "$ROOT/tools/ppm2png.py" "$B" "$IMG/${TAG}_displaced.png"   >/dev/null
echo -n "$TAG "
python3 "$ROOT/tools/snapshot_diff.py" "$A" "$B" "$IMG/${TAG}_dispdiff.png" | tr '\n' ' '; echo
rm -rf "$OUT"
