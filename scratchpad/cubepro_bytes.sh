#!/bin/zsh
# BYTE gate for --deferred_cube_prepass. Three arms, one worktree, one asset
# tree: the parent binary, the child with the flag off, the child with it on.
#
# Two sets, because they cover different things:
#   * the FIVE POSES of his acceptance arm — the only recipe that exercises the
#     flag's own configuration (PolyId + --shadow_dynamic + --hdr_linear);
#   * the nine 2026-08-16f pins — greets t=1588, fountain t=2500, city t=1961
#     both arms, chase x5 — which the flag mostly does NOT reach, and that is
#     stated rather than quoted as coverage.
#   usage: cubepro_bytes.sh <tag> <binary> [extra flags...]
set -u
WT=/Users/gil-ad/work/rev-cubetap
TAG=$1; BIN=$2; shift 2
EXTRA=($@)
OUT=${PIN_OUT:-/tmp/cube/bytes}/$TAG
rm -rf $OUT; mkdir -p $OUT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd $WT/Runtime
HIS=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace)
GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"

for pose in 5743 2845 6097 3409 5813; do
  ./$BIN --snapshot=greets@t=$pose --out=$OUT/arm$pose $HIS $EXTRA \
      --profiler=0 >/dev/null 2>&1
done
FDS_GREETS_CAM=$GREETS_CAM ./$BIN --snapshot=greets@t=1588 --out=$OUT/pin_greets \
    --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 \
    --profiler=0 --no-env_refl $EXTRA >/dev/null 2>&1
./$BIN --snapshot=fountain@t=2500 --out=$OUT/pin_fount --deferred --hdr \
    --glass-refract=1 --glass-test --profiler=0 $EXTRA >/dev/null 2>&1
FDS_CITY_ENV_PIXEL=1 ./$BIN --snapshot=city@t=1961 --out=$OUT/pin_city \
    --deferred --profiler=0 $EXTRA >/dev/null 2>&1
./$BIN --snapshot=city@t=1961 --out=$OUT/pin_cityarm --env_live_water --deferred \
    --city_env_pixel --profiler=0 $EXTRA >/dev/null 2>&1
./$BIN --snapshot=chase@t=100,400,800,1200,1600 --out=$OUT/pin_chase --deferred \
    --profiler=0 $EXTRA >/dev/null 2>&1

print -r -- "===== $TAG ($BIN ${EXTRA:-default}) ====="
for f in $OUT/**/*.ppm(N); do print -r -- "  $(md5 -q $f)  ${f#$OUT/}"; done
