#!/bin/zsh
# 13-pin gate battery, each row's recipe VERBATIM from docs/SESSION_STATE.md.
# $1 = binary path (must live in a Runtime/ whose assets are the worktree's).
set -u
BIN="$1"; RT="$(dirname "$BIN")"; OUT="${2:-/tmp/gatework}"
rm -rf "$OUT"; mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd "$RT" || exit 1
PASS=0; FAIL=0
chk() { # label expected got
  if [ "$3" = "$2" ]; then printf "PASS  %-24s %s\n" "$1" "$3"; PASS=$((PASS+1))
  else printf "FAIL  %-24s got %s want %s\n" "$1" "$3" "$2"; FAIL=$((FAIL+1)); fi
}
h() { md5 -q "$1"/*_color.ppm 2>/dev/null | tr '\n' ' ' ; }
d="$OUT/city"; mkdir -p "$d"
FDS_CITY_ENV_PIXEL=1 "$BIN" --snapshot=city@t=1961 --out="$d" --deferred >/dev/null 2>&1
chk city "bd4ffbf87d1492175a9b6c1111fb3f5f " "$(h $d)"
for t in 1961:4cb8d2ca68b72f8a24627f42077eef25 2400:f473fe2b2658fa0c1c290e1acf8265b9 400:d3374de6a0840a6927e00eb54b48b359; do
  tt=${t%%:*}; ex=${t##*:}; d="$OUT/ca$tt"; mkdir -p "$d"
  "$BIN" --snapshot=city@t=$tt --out="$d" --env_live_water --deferred --city_env_pixel >/dev/null 2>&1
  chk "city-acc t=$tt" "$ex " "$(h $d)"
done
d="$OUT/g1588"; mkdir -p "$d"
FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551" "$BIN" --snapshot=greets@t=1588 --out="$d" --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl >/dev/null 2>&1
chk "greets t=1588" "570a7b443f768393dc6647044a9e67b3 " "$(h $d)"
for t in 5743:440aa6bbb350ae95fbacf339dd2ad957 2845:00d17bc5610624bd1fea698c4b096979 6097:29c1e7fbd30e9ef811588a63d0778b7b 6133:bc1b0a8a703d6d6f6b3953eafc864d48; do
  tt=${t%%:*}; ex=${t##*:}; d="$OUT/ga$tt"; mkdir -p "$d"
  "$BIN" --snapshot=greets@t=$tt --out="$d" --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0 >/dev/null 2>&1
  chk "greets-acc t=$tt" "$ex " "$(h $d)"
done
d="$OUT/fount"; mkdir -p "$d"
"$BIN" --snapshot=fountain@t=2500 --out="$d" --deferred --hdr --glass-refract=1 --glass-test --profiler=0 >/dev/null 2>&1
chk fountain "8db68ccb59416e9a44037e9f387b7bd9 " "$(h $d)"
d="$OUT/chase"; mkdir -p "$d"
"$BIN" --snapshot=chase@t=100,400,800,1200,1600 --out="$d" --deferred >/dev/null 2>&1
chk "chase default x5" "b67b47f0de8b41365f96fff68e50d367 5bc199d4949a6212b4b7cb1004ab0e3a d1284b5a727bb6c5924b6ba3012f89ae 9c0f7c2fac7b8a1408f62110bb70d12f 9cdf5603f231392e64000ed2b850877a " "$(h $d)"
d="$OUT/chcin"; mkdir -p "$d"
"$BIN" --cinematic --deferred --snapshot=chase@t=800,1600 --out="$d" >/dev/null 2>&1
chk "chase cinematic x2" "d50a32d33f23a6de505257b663dbdc62 92ffa25d675a716c6809a7db133c3961 " "$(h $d)"
printf "\n== %d PASS / %d FAIL ==\n" $PASS $FAIL
