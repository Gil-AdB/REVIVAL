#!/usr/bin/env bash
# reflmir_gates.sh — the full pin battery for --refl_correct (2026-08-17).
#
# Recipes are VERBATIM from docs/SESSION_STATE.md's gates table. Run from the
# worktree root; binaries live in Runtime/ and are named on the command line.
#
#   ./scratchpad/reflmir_gates.sh DEMO_par par
#   ./scratchpad/reflmir_gates.sh DEMO_new new
#   ./scratchpad/reflmir_gates.sh DEMO_new off --no-refl_correct   # escape hatch
#
# THREE runs per recipe; RUN 1 IS DISCARDED (cold caches / first-touch pages
# give a false stable value — SESSION_STATE's city trap). Runs 2 and 3 must
# agree or the row prints MISMATCH.
set -u
BIN=${1:?binary}
TAG=${2:?tag}
EXTRA=${3:-}
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
SCR=/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad
OUT=$SCR/reflmir_gates
mkdir -p "$OUT"
RES=$OUT/$TAG.txt
: > "$RES"

# run3 <label> <env-prefix-or-empty> <args...>
run3() {
  local label="$1"; shift
  local envp="$1"; shift
  local h2="" h3="" i
  for i in 1 2 3; do
    local d; d=$(mktemp -d)
    if [ -n "$envp" ]; then
      env $envp ./"$BIN" "$@" --out="$d" $EXTRA >/dev/null 2>&1
    else
      ./"$BIN" "$@" --out="$d" $EXTRA >/dev/null 2>&1
    fi
    local rc=$?
    if [ $rc -ne 0 ]; then echo "$label RUN$i rc=$rc" >> "$RES"; fi
    # one line per emitted color ppm, sorted by name
    local hs; hs=$(cd "$d" && ls -1 *.ppm 2>/dev/null | sort | while read -r f; do
        printf "%s:%s " "${f%%_color.ppm}" "$(md5 -q "$f")"; done)
    [ $i -eq 2 ] && h2="$hs"
    [ $i -eq 3 ] && h3="$hs"
    rm -rf "$d"
  done
  if [ "$h2" = "$h3" ]; then echo "$label  STABLE  $h2" >> "$RES"
  else echo "$label  MISMATCH  r2=$h2  r3=$h3" >> "$RES"; fi
  tail -1 "$RES"
}

echo "### $TAG  bin=$BIN md5=$(md5 -q "$BIN")  extra='$EXTRA'" | tee -a "$RES"

run3 "chase-default"   "" --snapshot=chase@t=100,400,800,1200,1600 --deferred
run3 "chase-cinematic" "" --cinematic --deferred --snapshot=chase@t=800,1600
run3 "city-plain"      "FDS_CITY_ENV_PIXEL=1" --snapshot=city@t=1961 --deferred
run3 "city-accept1961" "" --snapshot=city@t=1961 --env_live_water --deferred --city_env_pixel
run3 "city-accept2400" "" --snapshot=city@t=2400 --env_live_water --deferred --city_env_pixel
run3 "fountain"        "" --snapshot=fountain@t=2500 --deferred --hdr --glass-refract=1 --glass-test --profiler=0
run3 "greets1588"      'FDS_GREETS_CAM=-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551' \
     --snapshot=greets@t=1588 --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl
for T in 5743 2845 6097 6133; do
  run3 "greets-acc$T"  "" --snapshot=greets@t=$T --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0
done

echo "--- $RES ---"
