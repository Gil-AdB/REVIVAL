#!/bin/zsh
# spotpyr_sweep.sh — the STALENESS gate for the 2-D spot tap's uniformity
# pyramid: 9 greets poses x 6 shadow configurations, per named binary.
# A pyramid that fails to track a re-baked plane shows up as a
# FRAME-DEPENDENT divergence, which one pose cannot see.
#
# ONE RUN PER POSE. A multi-t snapshot (`--snapshot=greets@t=a,b,c`) is NOT
# run-to-run stable past its FIRST pose — measured on the parent binary alone,
# poses 2..9 differ between two consecutive runs while pose 1 is identical, so
# that form cannot certify anything. Each pose gets its own process here.
#   usage: spotpyr_sweep.sh <binary-name-in-Runtime> <tag>
set -u
WT=/Users/gil-ad/work/rev-spotpyr
BIN=$1; TAG=$2
OUT=${SWEEP_OUT:-/tmp/spotpyr/sweep}/$TAG
rm -rf $OUT; mkdir -p $OUT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd $WT/Runtime
POSES=(1588 2000 3122 4871 5534 5743 5780 5814 5970)

one() {  # $1 = config label, $2 = pose, rest = extra literal flags
  local lbl=$1 t=$2; shift 2
  local d=$OUT/$lbl/$t; rm -rf $d; mkdir -p $d
  ./$BIN --snapshot=greets@t=$t --out=$d --deferred "$@" >/dev/null 2>&1
  print -r -- "$lbl t=$t  $(md5 -q $d/*.ppm)"
}

for t in $POSES; do one default     $t; done
for t in $POSES; do one swizzle     $t --shadow_swizzle; done
for t in $POSES; do one lmcomposite $t --shadow_lightmap --shadow_lm_dynamic; done
for t in $POSES; do one nodyn       $t --no-shadow_dynamic; done
for t in $POSES; do one nopcf       $t --shadow_polyid_no_pcf; done
for t in $POSES; do FDS_SHADOW_POLYID=0 one depthmode $t; done
