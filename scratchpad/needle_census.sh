#!/usr/bin/env bash
# 2026-08-16x: what the degenerate pre-reject removes, and whether a LOAD-TIME
# collinearity scan would have found any of it. Census build only
# (-DFDS_NEEDLE_CENSUS=1 -DFDS_REFLTN_CENSUS=1).
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
B=${B:-DEMO_census}
pose() { # tag, then argv
  local tag="$1"; shift
  for arm in "" "--needle_cull"; do
    local d; d=$(mktemp -d)
    echo "--- $tag  arm=[${arm:-off}]"
    ./"$B" "$@" --out="$d" $arm 2>&1 >/dev/null | grep -E "^\[REFLTN\]|^\[NEEDLE\]"
    rm -rf "$d"
  done
}
GACC="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0"
pose "chase t=100"      --snapshot=chase@t=100  --deferred
pose "chase t=800"      --snapshot=chase@t=800  --deferred
pose "city  t=1961"     --snapshot=city@t=1961  --deferred --profiler=0
pose "city  t=1961 arm" --snapshot=city@t=1961  --env_live_water --deferred --city_env_pixel --profiler=0
pose "fountain t=2500"  --snapshot=fountain@t=2500 --deferred --hdr --glass-refract=1 --glass-test --profiler=0
# shellcheck disable=SC2086
pose "greets t=5743"    --snapshot=greets@t=5743 $GACC
