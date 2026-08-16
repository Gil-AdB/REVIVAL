#!/bin/zsh
# Pin battery for the frame raster tile grid (--frame_tile_x/y).
#
# Two axes, ONE asset tree, ONE worktree:
#   BINARY  DEMO_par (parent) vs DEMO_tg at the DEFAULT grid -> must be
#           byte-NULL; that is what "default unchanged" means.
#   GRID    DEMO_tg 6x5 vs 12x10 / 6x20 / 24x20 -> NOT byte-null by
#           construction (tile boundaries move, so each face is clipped
#           against a different rect and MiplevelClipper sees a different
#           sub-polygon); this is what gets quantified for the judge call.
#
# Poses are the ACCEPTANCE arms, not bare --deferred.
#
# usage: tg_pins.sh <tag> <binary> [WxH]     e.g. tg_pins.sh g6x20 DEMO_tg 6x20
set -u
WT=/Users/gil-ad/work/rev-tilegrid
TAG=$1; BIN=$2; GRID=${3:-}
OUT=${PIN_OUT:-/tmp/tg/pins}/$TAG
rm -rf $OUT; mkdir -p $OUT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

G=()
if [[ -n $GRID ]]; then
  G=(--frame-tile-x=${GRID%x*} --frame-tile-y=${GRID#*x})
fi

cd $WT/Runtime
./$BIN --snapshot=chase@t=100,400,800,1200,1600 --out=$OUT/chase --deferred $G \
       >/dev/null 2>&1
./$BIN --snapshot=city@t=1961 --out=$OUT/city \
       --env_live_water --deferred --city_env_pixel $G >/dev/null 2>&1
./$BIN --snapshot=greets@t=5743 --out=$OUT/greets \
       --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
       --greets-displace $G >/dev/null 2>&1
./$BIN --snapshot=fountain@t=2500 --out=$OUT/fount --deferred --hdr \
       --glass-refract=1 --glass-test $G >/dev/null 2>&1

print -r -- "===== $TAG ($BIN grid=${GRID:-default}) ====="
for f in $OUT/**/*.ppm(N); do print -r -- "  $(md5 -q $f)  ${f#$OUT/}"; done
