#!/usr/bin/env bash
# reflmir_gen2.sh — publish the --refl_correct review images (2026-08-17 round,
# rebased parent/child pair DEMO_par 6acb2ebf / DEMO_new d3890c8b).
#
# chase frames: one 6-pose sweep per binary (the PERF_STATE chase arm).
# city  frames: FIVE CONSECUTIVE ticks per binary ending on the pose. This is
#   NOT decoration — city's water carries NO mirrored content on the first tick
#   of a process, so the one-pose-per-process pin recipe renders a frame in
#   which this flag provably cannot do anything (proved: 1 tick byte-identical,
#   2 ticks differ). Continuous play is the honest context for the look review.
set -u
WT=/Users/gil-ad/work/rev-reflmir
R=/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad
IMG=$WT/docs/img/reflmir
P="python3 $WT/scratchpad/reflmir_img.py"
mkdir -p "$IMG"

crop() {
  case $1 in
    chase_000100) echo "1280 320 620 420";;
    chase_000400) echo "1280 480 620 420";;
    chase_000800) echo "1280 560 620 420";;
    chase_001000) echo "520 480 620 420";;
    chase_001300) echo "480 320 620 420";;
    chase_001600) echo "880 640 620 420";;
    city_001961)  echo "360 560 700 460";;
    city_002400)  echo "400 600 700 460";;
  esac
}

for t in 000100 000400 000800 001000 001300 001600; do
  b=$R/reflmir2/before/chase_t${t}_color.ppm
  a=$R/reflmir2/after/chase_t${t}_color.ppm
  $P full  "$b" "$IMG/chase_t${t}_before.png" --maxw 1280
  $P full  "$a" "$IMG/chase_t${t}_after.png"  --maxw 1280
  $P where "$b" "$a" "$IMG/chase_t${t}_where.png" --maxw 1280
  set -- $(crop chase_$t)
  $P sbs "$b" "$a" "$IMG/chase_t${t}_crop.png" $1 $2 $3 $4
done

for t in 001961 002400; do
  b=$R/citywarm/before/city_t${t}_color.ppm
  a=$R/citywarm/after/city_t${t}_color.ppm
  $P full  "$b" "$IMG/city_t${t}_before.png" --maxw 1280
  $P full  "$a" "$IMG/city_t${t}_after.png"  --maxw 1280
  $P where "$b" "$a" "$IMG/city_t${t}_where.png" --maxw 1280
  set -- $(crop city_$t)
  $P sbs "$b" "$a" "$IMG/city_t${t}_crop.png" $1 $2 $3 $4
done

ls -1 "$IMG" | wc -l
du -sh "$IMG"
