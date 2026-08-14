#!/bin/zsh
# Cone-pass ablation ladder ON CHASE (round 6 of the cone campaign).
# Chase's cones are 32 of 34 NARROW -> the 8-segment hybrid (segPath), which
# has never had the ladder run on it. Same staging as scratchpad/cone_ablate.sh
# but driven through the SNAPSHOT harness, because chase has no --bench arm.
#
# usage: chase_ablate.sh [runs] [t] [snaps] [stages...]
set -u
WT=/Users/gil-ad/work/rev-chasecone
SRC=$WT/FDS/RENDER/DeferredVolumetric.cpp
RUNS=${1:-3}; POSE=${2:-800}; SNAPS=${3:-10}
(( $# >= 3 )) && shift 3 || shift $#
if (( $# )); then STAGES=($@); else STAGES=(0 1 2 3 4 5 6 7 8 9 10); fi
OUT=${ABL_OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/chase_ablate}
mkdir -p $OUT
TLIST=$(python3 -c "print(','.join(['$POSE']*$SNAPS))")

print -r -- "# chase cone ablation ladder t=$POSE snaps=$SNAPS runs=$RUNS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"

for st in $STAGES; do
  perl -pi -e "s/^#define FDS_CONE_ABLATE .*\$/#define FDS_CONE_ABLATE $st/" $SRC
  grep -q "^#define FDS_CONE_ABLATE $st\$" $SRC || { print -r -- "!! sed failed for stage $st"; exit 2; }
  cmake --build $WT/build >/dev/null 2>&1 || { print -r -- "!! build failed stage $st"; exit 2; }
  cp $WT/build/DEMO/DEMO $WT/Runtime/DEMO_abl
  codesign -f -s - $WT/Runtime/DEMO_abl >/dev/null 2>&1
  rm -f $OUT/s${st}_*.txt
  for r in $(seq 1 $RUNS); do
    (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO_abl \
      --snapshot=chase@t=$TLIST --out=$OUT/snap --deferred \
      --deferred_prof=1 --hw_prof --strict_flags 2>&1) \
      | grep -E '^\[DPROF\]' > $OUT/s${st}_r${r}.txt
  done
  print -r -- "# stage $st done (load $(uptime | sed 's/.*averages: //'))"
done

perl -pi -e "s/^#define FDS_CONE_ABLATE \\d+\$/#define FDS_CONE_ABLATE 0/" $SRC

python3 - "$OUT" <<'PY'
import sys, glob, re, os, collections
out = sys.argv[1]
acc = collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out, "s*_r*.txt"))):
    b = os.path.basename(f)[:-4].split("_")
    st, run = int(b[0][1:]), int(b[1][1:])
    if run == 1: continue            # discard run 1 after a rebuild
    for line in open(f):
        m = re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if not m: continue
        name = m.group(1).strip()
        if name not in ("renderFrame", "cones"): continue
        try:
            acc[st][name].append((float(m.group(3)), float(m.group(7)),
                                  float(m.group(8)), float(m.group(9))))
        except ValueError: pass

LABEL = {0: "full pass",
         1: "cut at spot-loop top (per-batch floor)",
         2: "+ per-spot scalar prologue",
         3: "+ cone-interval SOLVE (scalar arm on segPath)",
         4: "+ scalar per-lane dz/fade loop",
         5: "+ WHOLE integration body (pre-accumulate)",
         6: "  body: + broadcasts, alpha/beta/gamma, disc, rsqrt-NR, args",
         7: "  body: + atanDiff/8-SEGMENT LOOP -> vIntegral",
         8: "  body: + midpoint cone/fade/softEdge block",
         9: "  body: + vAcc chain (rcp-NR, N, muls, fog)",
        10: "  body: + per-lane NOISE loop"}
sts = sorted(acc)
print(f"\n{'stage':<6}{'what is KEPT':<46}{'cones Gi/f':>11}{'cones Gc/f':>11}{'wall_min':>10}{'frame Gi/f':>11}")
print("-"*95)
mins = {}
for st in sts:
    c = acc[st].get("cones"); rf = acc[st].get("renderFrame")
    if not c: continue
    gi = min(x[1] for x in c); gc = min(x[2] for x in c); w = min(x[0] for x in c)
    fi = min(x[1] for x in rf) if rf else float('nan')
    mins[st] = (gi, gc, w, fi)
    print(f"{st:<6}{LABEL.get(st,''):<46}{gi:>11.3f}{gc:>11.3f}{w:>10.2f}{fi:>11.3f}")

print(f"\n{'INCREMENT attributable to each stage':<52}{'Gi/f':>9}{'% pass':>8}{'Gc/f':>9}{'% cyc':>8}")
print("-"*86)
full = mins.get(0, (float('nan'), float('nan')))[0]
fullc = mins.get(0, (float('nan'), float('nan')))[1]
order = [(1, "per-batch floor (prologue+composite, no spot loop)"),
         (2, "per-spot loop + scalar prologue"),
         (3, "the cone-interval SOLVE"),
         (4, "scalar per-lane dz/fade loop"),
         (6, "body: broadcasts + quadratic + rsqrt-NR + args"),
         (7, "body: atanDiff / THE 8-SEGMENT LOOP"),
         (8, "body: midpoint cone/fade/softEdge"),
         (9, "body: vAcc chain (rcp-NR + muls + fog)"),
         (10,"body: per-lane NOISE loop"),
         (5, "body: masks + midpoint shadow tap")]
prev = 0.0; prevc = 0.0
for st, lab in order:
    if st not in mins: continue
    cur, curc = mins[st][0], mins[st][1]
    inc = cur - prev if st > 1 else cur
    incc = curc - prevc if st > 1 else curc
    if st == 6 and 4 in mins:
        inc = cur - mins[4][0]; incc = curc - mins[4][1]
    print(f"{lab:<52}{inc:>9.3f}{100*inc/full:>7.1f}%{incc:>9.3f}{100*incc/fullc:>7.1f}%")
    prev = cur; prevc = curc
if 5 in mins and 0 in mins:
    inc = mins[0][0] - mins[5][0]; incc = mins[0][1] - mins[5][1]
    print(f"{'per-lane colour ACCUMULATE':<52}{inc:>9.3f}{100*inc/full:>7.1f}%{incc:>9.3f}{100*incc/fullc:>7.1f}%")
print(f"{'TOTAL (= full pass)':<52}{full:>9.3f}{100.0:>7.1f}%{fullc:>9.3f}{100.0:>7.1f}%")
PY
