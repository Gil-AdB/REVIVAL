#!/bin/zsh
# RECIPE (b) — allocation audit. docs/HW_PROFILING.md §4.
#
# Instruments' Allocations/VM Tracker templates are unreachable on this box (no
# Xcode.app => no xctrace). The Command Line Tools ship the same underlying
# machinery as separate binaries, and they work on a LIVE pid — so this launches
# the demo, lets it get past init and the lazy first-frame bakes, then snapshots
# it from outside.
#
#   usage: hwprof_alloc.sh [scene] [pose] [settle-seconds]
#   e.g.   hwprof_alloc.sh greets 5743 14
#
# Run from Runtime/. Writes a report set to $OUT (default /tmp/hwprof_alloc).
#
# COMPLEMENTS --mem_census, it does not replace it: --mem_census knows what each
# engine buffer IS and which variable its size scales with; this knows only
# bytes and call stacks, but it sees allocations nobody has taught the census
# about — which is exactly what the census's UNCENSUSED RESIDUAL line is for.
# Read them together: the residual says how much is unexplained, this says where
# it came from.
set -u
SCENE=${1:-greets}; POSE=${2:-5743}; SETTLE=${3:-14}
BIN=${HWPROF_BIN:-./DEMO}
OUT=${HWPROF_OUT:-/tmp/hwprof_alloc}
mkdir -p $OUT

print -r -- "# load: $(uptime | sed 's/.*averages: //')"

# ── peak footprint + page faults for the whole run (cheap, no stack logging) ──
print; print -r -- "# ---- /usr/bin/time -l : peak RSS, faults, context switches ----"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy /usr/bin/time -l "$BIN" \
  --bench=scene@scene=$SCENE,t=$POSE,iters=8 \
  --deferred --texture_filter=1 --strict_flags >/dev/null 2>$OUT/time.txt
grep -E "maximum resident|page reclaims|page faults|involuntary|voluntary|peak memory" \
  $OUT/time.txt | tee $OUT/summary_time.txt

# ── live snapshot with stack logging on ─────────────────────────────────────
# MallocStackLogging makes every live allocation carry its call stack, which is
# what turns "0.5 GB resident" into "0.5 GB FROM HERE".
print; print -r -- "# ---- live snapshot after ${SETTLE}s (past init + first-frame bakes) ----"
MallocStackLogging=1 MallocStackLoggingNoCompact=1 \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" \
  --bench=scene@scene=$SCENE,t=$POSE,iters=400 \
  --deferred --texture_filter=1 --strict_flags >/dev/null 2>&1 &
PID=$!
sleep $SETTLE
if ! kill -0 $PID 2>/dev/null; then
  print -r -- "!! process exited before the snapshot — raise iters or lower SETTLE"
  exit 1
fi

/usr/bin/footprint -a $PID              > $OUT/footprint.txt 2>&1
/usr/bin/vmmap -summary $PID            > $OUT/vmmap_summary.txt 2>&1
/usr/bin/vmmap $PID                     > $OUT/vmmap_full.txt 2>&1
/usr/bin/heap $PID                      > $OUT/heap.txt 2>&1
/usr/bin/malloc_history $PID -allBySize > $OUT/malloc_bysize.txt 2>&1
/usr/bin/leaks $PID                     > $OUT/leaks.txt 2>&1
kill $PID 2>/dev/null; wait $PID 2>/dev/null

print; print -r -- "# ---- vmmap: biggest regions ----"
grep -E "^(Physical footprint|MALLOC_|VM_ALLOCATE|__DATA|Dirty Size)" $OUT/vmmap_summary.txt | head -20
awk '/REGION TYPE/,/^$/' $OUT/vmmap_summary.txt | head -25

print; print -r -- "# ---- heap: largest malloc classes ----"
head -18 $OUT/heap.txt

print; print -r -- "# ---- malloc_history: biggest live allocation stacks ----"
head -40 $OUT/malloc_bysize.txt

print; print -r -- "# ---- leaks ----"
grep -E "total leaked|leaks for" $OUT/leaks.txt | head -5

print; print -r -- "# full reports in $OUT/"
