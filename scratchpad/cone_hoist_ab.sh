#!/bin/zsh
# Round 7 A/B driver — the per-(tile x spot) invariant hoist.
#
# Two arms, TWO BINARIES, ONE asset tree (docs/HW_PROFILING.md §5): a
# one-binary flag A/B would price a flag, and this change ships without one
# (§14.3: the dual-arm tax in this kernel is +5.9% instructions for one live
# bool). Interleaved via scratchpad/prof1.py with the order rotating each
# round and round 0 discarded.
#
#   usage: cone_hoist_ab.sh <parent-binary> <new-binary> [rounds] [outdir]
#
# Build the parent binary from the parent commit IN THE SAME WORKTREE, then
# rebuild with the change; copy each to Runtime/ and `codesign -f -s -` (a
# copied+unsigned binary dies with rc=137 on arm64 macOS).
set -u
PARENT=${1:?parent binary}; NEW=${2:?new binary}; ROUNDS=${3:-7}
OUT=${4:-/tmp/cone_hoist}
PROF=${PROF1:-$(cd "$(dirname $0)" && pwd)/prof1.py}
SINK=$OUT/snapsink
mkdir -p $SINK

GC_PIN="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"
GC_HIS="-8.6249094,2.72651696,-53.2339516,0.210607708,0.0055912463,-0.977554619"

# chase has no --bench arm, so its poses go through the SNAPSHOT harness with
# the same t repeated 10x; city and greets use --bench.
gen () {   # $1 tag  $2 python-list-of-args  $3 env-json
  python3 - "$OUT/arms_$1.json" "$PARENT" "$NEW" "$2" "$3" <<'PY'
import json, sys
path, parent, new, args, env = sys.argv[1:6]
a = json.loads(args); e = json.loads(env)
json.dump([{"tag": "parent", "bin": parent, "args": a, "env": e},
           {"tag": "hoist",  "bin": new,    "args": a, "env": e}],
          open(path, "w"), indent=1)
PY
}

CHASE800="[\"--snapshot=chase@t=800,800,800,800,800,800,800,800,800,800\",\"--out=$SINK\",\"--deferred\",\"--deferred_prof=1\",\"--hw_prof\",\"--strict_flags\"]"
CHASE400="[\"--snapshot=chase@t=400,400,400,400,400,400,400,400,400,400\",\"--out=$SINK\",\"--deferred\",\"--deferred_prof=1\",\"--hw_prof\",\"--strict_flags\"]"
CITY='["--bench=scene@scene=city,t=1961,iters=20,xres=1920,yres=1080","--deferred","--deferred_prof=1","--hw_prof","--strict_flags"]'
GRPIN='["--bench=scene@scene=greets,t=1588,iters=4","--deferred","--hdr","--profiler=1","--deferred_prof=1","--hw_prof","--strict_flags"]'
GRHIS='["--bench=scene@scene=greets,t=3122,iters=20,xres=1512,yres=848","--greets_displace","--deferred_prof=1","--hw_prof","--strict_flags"]'

gen chase_t800    "$CHASE800" '{}'
gen chase_t400    "$CHASE400" '{}'
gen city_t1961    "$CITY"     '{}'
gen greets_t1588  "$GRPIN"    "{\"FDS_GREETS_CAM\":\"$GC_PIN\"}"
gen greets_t3122  "$GRHIS"    "{\"FDS_GREETS_CAM\":\"$GC_HIS\"}"

for pose in chase_t800 chase_t400 city_t1961 greets_t1588 greets_t3122; do
  print -r -- "########## $pose  load=$(uptime | sed 's/.*averages: //')"
  python3 $PROF run $OUT/$pose $OUT/arms_$pose.json $ROUNDS
done
for pose in chase_t800 chase_t400 city_t1961 greets_t1588 greets_t3122; do
  print -r -- "##### $pose"
  python3 $PROF report $OUT/$pose --phases cones,renderFrame
done
