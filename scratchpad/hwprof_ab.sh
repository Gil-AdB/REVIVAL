#!/bin/zsh
# RECIPE (c) — diff two builds with hardware counters. docs/HW_PROFILING.md §5.
#
# Interleaved A/B of two DEMO binaries on one pose, reporting per-phase wall_min
# AND the hardware counters (--hw_prof). Arms alternate ABBA so a load ramp
# cannot land on one arm; IPC is the column to trust when the box is busy,
# because a descheduled worker burns wall time but retires no instructions.
#
#   usage: hwprof_ab.sh <binA> <binB> [rounds] [pose] [scene] [iters]
#
# Run it from the Runtime/ directory whose assets BOTH binaries should use — a
# commit-to-commit A/B must share one asset tree or it is not a matched pair.
# EVERY run's raw [DPROF] table is written to $OUT/ before anything is parsed,
# so a reporting bug costs a re-parse (hwprof_ab_report.py) and never a re-run.
set -u
BIN_A=${1:?binA}; BIN_B=${2:?binB}; ROUNDS=${3:-6}; POSE=${4:-5743}
SCENE=${5:-greets}; ITERS=${6:-20}
OUT=${HWPROF_OUT:-/tmp/hwprof_ab}
mkdir -p $OUT; rm -f $OUT/r*_*.txt

run() {  # $1 = round, $2 = arm label, $3 = binary
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$3" \
    --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
    --deferred --texture_filter=1 --profiler=1 \
    --deferred_prof=1 --hw_prof --strict_flags 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt
}

print -r -- "# hwprof A/B  scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS"
print -r -- "# A=$BIN_A"
print -r -- "# B=$BIN_B"
print -r -- "# out=$OUT"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  if (( r % 2 )); then run $r A "$BIN_A"; run $r B "$BIN_B"
  else                 run $r B "$BIN_B"; run $r A "$BIN_A"; fi
  print -r -- "# round $r done  (load $(uptime | sed 's/.*averages: //'))"
done
python3 "${0:h}/hwprof_ab_report.py" $OUT
