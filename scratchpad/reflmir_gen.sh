#!/usr/bin/env bash
# reflmir_gen.sh — publish the --refl_correct review images into docs/img/reflmir.
set -u
WT=/Users/gil-ad/work/rev-reflmir
D=/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/reflmir
IMG=$WT/docs/img/reflmir
P="python3 $WT/scratchpad/reflmir_img.py"
mkdir -p "$IMG"

# chase: hot windows found by reflmir_hot.py (x y w h)
chase_crop() {
  case $1 in
    000100) echo "1360 360 560 380";;
    000400) echo "1280 480 560 380";;
    000800) echo "1360 600 560 380";;
    001000) echo "560 480 560 380";;
    001300) echo "1360 480 560 380";;
    001600) echo "840 610 260 280";;
  esac
}

for t in 000100 000400 000800 001000 001300 001600; do
  b=$D/before/chase_t${t}_color.ppm
  a=$D/after/chase_t${t}_color.ppm
  n=$D/norefl/chase_t${t}_color.ppm
  $P full  "$b" "$IMG/chase_t${t}_before.png" --maxw 1280
  $P full  "$a" "$IMG/chase_t${t}_after.png"  --maxw 1280
  $P where "$b" "$a" "$IMG/chase_t${t}_where.png" --maxw 1280
  set -- $(chase_crop $t)
  $P tri "$n" "$b" "$a" "$IMG/chase_t${t}_crop3.png" $1 $2 $3 $4
done

# city — PLAIN arm (`--deferred`), which is the SHIPPING configuration
for t in 001961 002400; do
  b=$D/before_plain/city_t${t}_color.ppm
  a=$D/after_plain/city_t${t}_color.ppm
  $P full  "$b" "$IMG/city_t${t}_plain_before.png" --maxw 1280
  $P full  "$a" "$IMG/city_t${t}_plain_after.png"  --maxw 1280
  $P where "$b" "$a" "$IMG/city_t${t}_plain_where.png" --maxw 1280
done

ls -1 "$IMG" | wc -l
du -sh "$IMG"
