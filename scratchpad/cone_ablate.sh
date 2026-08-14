#!/bin/zsh
# Cone-pass ablation ladder, round 2 (against the 8-wide-solve arm).
# For each stage n: rewrite the FDS_CONE_ABLATE default in
# DeferredVolumetric.cpp, rebuild that TU + relink, copy the binary into this
# worktree's Runtime/, and bench city t=1961 with --hw_prof. Instruction counts
# are deterministic for a fixed binary, so RUNS=3 with run 1 discarded is ample.
#
# usage: cone_ablate.sh [runs] [t] [iters] [scene] [stages...]
set -u
WT=/Users/gil-ad/work/rev-conevec
SRC=$WT/FDS/RENDER/DeferredVolumetric.cpp
RUNS=${1:-3}; POSE=${2:-1961}; ITERS=${3:-6}; SCENE=${4:-city}
(( $# >= 4 )) && shift 4 || shift $#
if (( $# )); then STAGES=($@); else STAGES=(0 1 2 3 4 5 6 7 8 9 10); fi
OUT=${ABL_OUT:-/tmp/cone_ablate}
mkdir -p $OUT

print -r -- "# cone ablation ladder scene=$SCENE t=$POSE iters=$ITERS runs=$RUNS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"

for st in $STAGES; do
  perl -pi -e "s/^#define FDS_CONE_ABLATE .*\$/#define FDS_CONE_ABLATE $st/" $SRC
  grep -q "^#define FDS_CONE_ABLATE $st\$" $SRC || { print -r -- "!! sed failed for stage $st"; exit 2; }
  cmake --build $WT/build >/dev/null 2>&1 || { print -r -- "!! build failed stage $st"; exit 2; }
  cp $WT/build/DEMO/DEMO $WT/Runtime/DEMO
  rm -f $OUT/s${st}_*.txt
  for r in $(seq 1 $RUNS); do
    (cd $WT/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO \
      --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
      --deferred --texture_filter=1 --profiler=1 \
      --deferred_prof=1 --hw_prof --strict_flags 2>&1) \
      | grep -E '^\[DPROF\]' > $OUT/s${st}_r${r}.txt
  done
  print -r -- "# stage $st done (load $(uptime | sed 's/.*averages: //'))"
done

# restore shipping default
perl -pi -e "s/^#define FDS_CONE_ABLATE \\d+$/#define FDS_CONE_ABLATE 0/" $SRC

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
         3: "+ 8-wide cone-interval solve",
         4: "+ scalar per-lane dz/fade loop",
         5: "+ WHOLE integration body (pre-accumulate)",
         6: "  body: + broadcasts, alpha/beta/gamma, disc, rsqrt-NR, args",
         7: "  body: + atanDiff -> vIntegral",
         8: "  body: + midpoint cone/fade/softEdge block",
         9: "  body: + vAcc chain (rcp-NR, N, muls, fog)",
        10: "  body: + per-lane NOISE loop"}
sts = sorted(acc)
print(f"\n{'stage':<6}{'what is KEPT':<42}{'cones Gi/f':>11}{'cones Gc/f':>11}{'wall_min':>10}{'frame Gi/f':>11}")
print("-"*91)
mins = {}
for st in sts:
    c = acc[st].get("cones"); rf = acc[st].get("renderFrame")
    if not c: continue
    gi = min(x[1] for x in c); gc = min(x[2] for x in c); w = min(x[0] for x in c)
    fi = min(x[1] for x in rf) if rf else float('nan')
    mins[st] = (gi, gc, w, fi)
    print(f"{st:<6}{LABEL.get(st,''):<42}{gi:>11.3f}{gc:>11.3f}{w:>10.2f}{fi:>11.3f}")

print(f"\n{'INCREMENT (Ginstr/f) attributable to each stage':<52}{'Gi/f':>9}{'% of pass':>11}")
print("-"*72)
full = mins.get(0, (float('nan'),))[0]
order = [(1, "per-batch floor (prologue+composite, no spot loop)"),
         (2, "per-spot loop + scalar prologue"),
         (3, "the 8-wide cone-interval SOLVE"),
         (4, "scalar per-lane dz/fade loop"),
         (6, "body: broadcasts + quadratic + rsqrt-NR + args"),
         (7, "body: atanDiff (atan_approx_x8 + div)"),
         (8, "body: midpoint cone/fade/softEdge"),
         (9, "body: vAcc chain (rcp-NR + muls + fog)"),
         (10,"body: per-lane NOISE loop  <-- scalar, per-BATCH invariant"),
         (5, "body: masks + midpoint shadow tap")]
prev = 0.0
for st, lab in order:
    pass
prev = 0.0
for st, lab in order:
    if st not in mins: continue
    cur = mins[st][0]
    inc = cur - prev if st > 1 else cur
    if st == 6: inc = cur - mins[4][0]
    print(f"{lab:<52}{inc:>9.3f}{100*inc/full:>10.1f}%")
    prev = cur
if 5 in mins and 0 in mins:
    inc = mins[0][0] - mins[5][0]
    print(f"{'per-lane colour ACCUMULATE':<52}{inc:>9.3f}{100*inc/full:>10.1f}%")
print(f"{'TOTAL (= full pass)':<52}{full:>9.3f}{100.0:>10.1f}%")
PY
