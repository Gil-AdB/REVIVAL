#!/bin/zsh
# Five-pose greets snapshot battery under HIS acceptance arm (the pin recipe
# does NOT exercise it). One pose per process — a multi-t sweep is not
# run-to-run stable past its first pose.
#   usage: w2_poses.sh /path/to/DEMO tag [xres yres]
set -u
WT=/Users/gil-ad/work/rev-w2oct
BIN=$1; TAG=$2
OUT=${POSE_OUT:-/tmp/w2oct/poses}/$TAG
rm -rf $OUT; mkdir -p $OUT
cp $BIN $WT/Runtime/DEMO
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd $WT/Runtime
for t in 2845 3409 5743 5813 6097; do
  ./DEMO --snapshot=greets@t=$t --out=$OUT/g$t \
      --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
      --greets-displace --profiler=0 >/dev/null 2>&1
done
for f in $(find $OUT -name '*.ppm' -o -name '*.PPM' | sort); do
  print -r -- "  $(md5 -q $f)  ${f:t}"
done
