#!/usr/bin/env bash
# Pin gates for the xpar strip-extent work. Run from the worktree Runtime/.
# LITERAL flags only. Dummy drivers always. Run 1 discarded per the documented
# cold-cache artifact.
set -u
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT=/tmp/xparpins
mkdir -p "$OUT"

ARM="$1"          # "on" | "off"
if [ "$ARM" = "on" ]; then FLAG="--xpar_strip_extent"; FLAG2="--xpar_peel_early_out";
elif [ "$ARM" = "peelonly" ]; then FLAG="--no-xpar_strip_extent"; FLAG2="--xpar_peel_early_out";
else FLAG="--no-xpar_strip_extent"; FLAG2="--no-xpar_peel_early_out"; fi

echo "########## arm=$ARM ##########"

echo "--- fountain t=2500 (pin 8db68ccb...) x3, run1 discarded ---"
for r in 1 2 3; do
  rm -rf "$OUT/f$r"; mkdir -p "$OUT/f$r"
  ./DEMO --snapshot=fountain@t=2500 --out="$OUT/f$r" --deferred --hdr \
         --glass-refract=1 --glass-test --profiler=0 "$FLAG" "$FLAG2" >/dev/null 2>&1
  echo "run$r $(cat "$OUT/f$r"/*.ppm 2>/dev/null | md5 -q)"
done

echo "--- fountain t=1200 (the profile pose) x3 ---"
for r in 1 2 3; do
  rm -rf "$OUT/g$r"; mkdir -p "$OUT/g$r"
  ./DEMO --snapshot=fountain@t=1200 --out="$OUT/g$r" --deferred --hdr \
         --glass-refract=1 --glass-test --profiler=0 "$FLAG" "$FLAG2" >/dev/null 2>&1
  echo "run$r $(cat "$OUT/g$r"/*.ppm 2>/dev/null | md5 -q)"
done

echo "--- fountain t=1200 plain deferred (no glass -> plain TBR_Render path) x2 ---"
for r in 1 2; do
  rm -rf "$OUT/h$r"; mkdir -p "$OUT/h$r"
  ./DEMO --snapshot=fountain@t=1200 --out="$OUT/h$r" --deferred --profiler=0 "$FLAG" "$FLAG2" >/dev/null 2>&1
  echo "run$r $(cat "$OUT/h$r"/*.ppm 2>/dev/null | md5 -q)"
done

echo "--- greets t=1588 (differential; committed FLD in this worktree) x2 ---"
for r in 1 2; do
  rm -rf "$OUT/r$r"; mkdir -p "$OUT/r$r"
  FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551" \
  ./DEMO --snapshot=greets@t=1588 --out="$OUT/r$r" --deferred --hdr --glass-refract=1 \
         --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl "$FLAG" "$FLAG2" >/dev/null 2>&1
  echo "run$r $(cat "$OUT/r$r"/*.ppm 2>/dev/null | md5 -q)"
done

echo "--- city t=1961 (differential) x2 ---"
for r in 1 2; do
  rm -rf "$OUT/c$r"; mkdir -p "$OUT/c$r"
  FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out="$OUT/c$r" --deferred "$FLAG" "$FLAG2" >/dev/null 2>&1
  echo "run$r $(cat "$OUT/c$r"/*.ppm 2>/dev/null | md5 -q)"
done

echo "--- chase t=100,400,800,1200,1600 (differential) ---"
rm -rf "$OUT/ch"; mkdir -p "$OUT/ch"
./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out="$OUT/ch" --deferred "$FLAG" "$FLAG2" >/dev/null 2>&1
for f in "$OUT/ch"/*.ppm; do echo "  $(basename "$f") $(md5 -q "$f")"; done
