#!/bin/zsh
# RECIPE (a) — per-stage wall time + hardware counters on one bench pose.
# docs/HW_PROFILING.md §3.
#
#   usage: hwprof_stage.sh [scene] [pose] [iters] [extra DEMO flags...]
#   e.g.   hwprof_stage.sh greets 5743 20
#          hwprof_stage.sh city   1961 20 --hdr
#
# Run from Runtime/. Needs a DEMO built from a tree carrying --hw_prof.
# Emits the [DPROF] table with Ginstr/f, Gcyc/f and IPC columns appended, plus
# `sample`-based symbol attribution for the same run (time-based, not counters —
# `sample` is a stack sampler, the only symbol-level attribution reachable
# without Instruments).
set -u
SCENE=${1:-greets}; POSE=${2:-5743}; ITERS=${3:-20}; shift 3 2>/dev/null || true
BIN=${HWPROF_BIN:-./DEMO}
OUT=${HWPROF_OUT:-/tmp/hwprof}
mkdir -p $OUT

print -r -- "# load: $(uptime | sed 's/.*averages: //')"
print -r -- "# bin:  $BIN"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" \
  --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
  --deferred --texture_filter=1 --profiler=1 \
  --deferred_prof=1 --hw_prof --strict_flags "$@" 2>&1 \
  | grep -E '^\[DPROF\]' | tee $OUT/${SCENE}_${POSE}_dprof.txt

# ── symbol attribution for the same workload ────────────────────────────────
# `sample` needs the process alive, so launch, let it get past init + the lazy
# first-frame bakes, then sample the steady state.
print -r -- "\n# ---- sample(1) symbol profile, steady state ----"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" \
  --bench=scene@scene=$SCENE,t=$POSE,iters=$(( ITERS * 12 )) \
  --deferred --texture_filter=1 --deferred_prof=1 --strict_flags "$@" \
  >/dev/null 2>&1 &
PID=$!
sleep 12                                  # past init, env/SH bakes, mip first-touch
/usr/bin/sample $PID 8 1 -file $OUT/${SCENE}_${POSE}_sample.txt >/dev/null 2>&1
kill $PID 2>/dev/null; wait $PID 2>/dev/null

print -r -- "# heaviest leaf symbols (self time):"
awk '/^ *Sort by top of stack/,/^$/' $OUT/${SCENE}_${POSE}_sample.txt | head -25
print -r -- "\n# full table: $OUT/${SCENE}_${POSE}_sample.txt"
print -r -- "# NOTE: sample attributes WALL time on sampled stacks, not cycles or"
print -r -- "# misses. Cross-read it with the IPC column above: a symbol that is hot"
print -r -- "# in sample AND sits in a low-IPC phase is the memory-bound one."
