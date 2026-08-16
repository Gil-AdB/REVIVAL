#!/usr/bin/env bash
# 2026-08-16x A/B ladder for --needle_cull. ONE binary, three arms — off / on /
# floor (a second `off`, which is the noise bar) — interleaved and rotated per
# round, min over rounds. city + greets go through the --bench=scene harness at
# 1512x848; chase has no bench arm, so it goes through the SNAPSHOT harness with
# a repeated timestamp (the same staging 2026-08-14's chase_ablate.sh used).
#   usage: needle_ladder.sh <rounds> <scene> <t>
set -u
R="${1:-7}"; SCENE="${2:-city}"; T="${3:-1961}"
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
B=${B:-DEMO_needle}
ITERS=${ITERS:-24}
WARM=${WARM:-4}
SNAPS=${SNAPS:-10}
SINK=$(mktemp -d)
trap 'rm -rf "$SINK"' EXIT

case "$SCENE" in
  city)   FLAGS="--env_live_water --deferred --city_env_pixel" ;;
  greets) FLAGS="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace" ;;
  chase)  FLAGS="--deferred" ;;
  *)      echo "unknown scene $SCENE"; exit 2 ;;
esac

run() { # $1 = extra flags
  local extra="$1"
  if [ "$SCENE" = chase ]; then
    local tl; tl=$(python3 -c "print(','.join(['$T']*$SNAPS))")
    # shellcheck disable=SC2086
    ./"$B" --snapshot=chase@t="$tl" --out="$SINK" $FLAGS $extra \
        --deferred_prof=1 --hw_prof --strict_flags 2>&1 | awk '
      /^\[DPROF\] renderFrame/ { rfw=$4; rfi=$9; rfc=$10 }
      /^\[DPROF\]   gbuffer/   { gbw=$4; gbi=$9 }
      /^\[DPROF\]   clip/      { clw=$4; cli=$9 }
      END { printf "%s,%s,%s,%s,%s\n", rfw, rfi, rfc, gbw, gbi }'
  else
    # shellcheck disable=SC2086
    ./"$B" --bench=scene@scene="$SCENE",t="$T",iters="$ITERS",xres=1512,yres=848 \
        $FLAGS $extra --profiler=1 --deferred_prof="$WARM" --hw_prof 2>&1 | awk '
      /frame_ms min\/p50/ { split($0,a,"= "); split(a[2],b,"/"); fmin=b[1] }
      /^TOTL/             { totl=$2 }
      /^\[DPROF\] renderFrame/ { rfw=$4; rfi=$9; rfc=$10 }
      /^RNDR/             { rndr=$4 }
      /^XFRM/             { xfrm=$4 }
      END { printf "%s,%s,%s,%s,%s,%s,%s\n", fmin, totl, rfw, rfi, rfc, rndr, xfrm }'
  fi
}

ARMS=("off:" "on:--needle_cull" "floor:")
N=${#ARMS[@]}
for ((r=0; r<R; r++)); do
  for ((k=0; k<N; k++)); do
    idx=$(( (k + r) % N ))
    a="${ARMS[$idx]}"
    tag="${a%%:*}"; extra="${a#*:}"
    echo "$r,$tag,$(run "$extra")"
  done
done
