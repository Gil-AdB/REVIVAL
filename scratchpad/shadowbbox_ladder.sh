#!/usr/bin/env bash
# Order-ROTATED N-way ladder for the SHADOW-BAKE round. One pose per process.
#
# Why not scratchpad/ladder.sh: the shadow depth raster does NOT run inside
# renderFrame — `shadow-join` is 0.002 ms, the bake is its own stage before it —
# so ladder.sh's renderFrame/gbuffer columns are BLIND to this change by
# construction. They are still captured here, as the control that says the
# change stayed out of the frame path. The column that carries the result is
# [SHADOW-BAKE], printed per FDS_SHADOW_PROF_INTERVAL frames and split per mode
# (DynOmnis / DynMeshes); we take the MIN over the lines a run prints.
#
#   shadowbbox_ladder.sh <rounds> <scene> <t> "<bin1 bin2 ...>" -- <flags...>
# Per-arm extra flags: set LADDER_EXTRA_<bin-with-nonalnum-as-_> in the env.
# [SHADOW-PROF] additionally splits the bake into xform vs RASTER, and the
# raster half is the one this change touches — carry it as its own column so a
# move in the bake total can be attributed instead of asserted.
# CSV: round,arm,framemin,totl,dynomni_ms,dynmesh_ms,rast_omni,rast_mesh,xf_mesh,rf_wall,rf_ginstr,rf_gcyc,gbuf_ginstr
set -u
R="$1"; SCENE="$2"; T="$3"; ARMS=($4); shift 4; [ "${1:-}" = "--" ] && shift
FLAGS=("$@")
cd "$(dirname "${BASH_SOURCE[0]}")/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export FDS_SHADOW_PROF_INTERVAL=${SPI:-5}
ITERS=${ITERS:-24}
WARM=${WARM:-4}
run() {
  # arm name is "<binary>[:extra flags]"
  local arm="$1" bin extra
  bin="${arm%%:*}"
  extra="${arm#*:}"; [ "$extra" = "$arm" ] && extra=""
  # shellcheck disable=SC2086
  ./"$bin" --bench=scene@scene="$SCENE",t="$T",iters="$ITERS",xres=1512,yres=848 \
     "${FLAGS[@]}" $extra --profiler=1 --deferred_prof="$WARM" --hw_prof \
     --shadow-bake-time --shadow-prof 2>&1 | awk '
    function num(s) { gsub(/[^0-9.]/, "", s); return s + 0 }
    /frame_ms min\/p50/ { split($0,a,"= "); split(a[2],b,"/"); fmin=b[1] }
    /^TOTL/             { totl=$2 }
    /^\[SHADOW-BAKE\] dynamic bake/ {
      if ($0 ~ /DynOmnis/)  { if (do_m=="" || $4+0 < do_m+0) do_m=$4 }
      if ($0 ~ /DynMeshes/) { if (dm_m=="" || $4+0 < dm_m+0) dm_m=$4 }
    }
    /^\[SHADOW-PROF\]/ {
      # ... <mode>: xform=A raster=B sum=C  per-light: xform=D raster=E ...
      # STOP at "per-light:" — those fields carry the same two names and
      # silently overwrote the totals in the first cut of this script.
      x = ""; rr = ""
      for (i = 1; i <= NF; ++i) {
        if ($i == "per-light:") break
        if ($i ~ /^xform=/)  x  = num($i)
        if ($i ~ /^raster=/) rr = num($i)
      }
      if ($0 ~ /DynOmnis/)  { if (ro=="" || rr < ro+0) ro = rr }
      if ($0 ~ /DynMeshes/) { if (rm=="" || rr < rm+0) rm = rr; if (xm=="" || x < xm+0) xm = x }
    }
    /^\[DPROF\] renderFrame/ { rfw=$4; rfi=$9; rfc=$10 }
    /^\[DPROF\]   gbuffer/   { gbi=$9 }
    END { printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                 fmin, totl, do_m, dm_m, ro, rm, xm, rfw, rfi, rfc, gbi }'
}
N=${#ARMS[@]}
for ((r=0; r<R; r++)); do
  for ((k=0; k<N; k++)); do
    idx=$(( (k + r) % N ))
    a="${ARMS[$idx]}"
    echo "$r,$a,$(run "$a")"
  done
done
