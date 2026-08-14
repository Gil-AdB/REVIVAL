#!/bin/zsh
# THREE-ARM interleaved round-robin A/B for the cone stage fusion (round 3).
#
# BUILD THE ARMS FIRST, all into <worktree>/Runtime/ (DEMO resolves assets from
# its OWN directory, so an arm parked elsewhere renders nothing):
#   DEMO_parent  : the parent commit, plain build
#   DEMO_new     : the candidate, plain build
#   DEMO_force2  : -DFDS_CONE_FORCE=1                     (single-arm, unfused)
#   DEMO_force1  : -DFDS_CONE_FORCE=1 -DFDS_CONE_FUSE=1   (single-arm, fused)
# codesign -f -s - each one after copying, or macOS kills it (rc=137).
# The F2/F1 pair is the one that prices the FUSION; PAR/OFF/ON prices the COMMIT,
# and per round 2's method finding the OFF arm is not a stand-in for the parent.
# Arms: parent-commit binary / new binary flag OFF / new binary flag ON,
# plus the two single-arm control builds (FDS_CONE_FORCE=2 pre-fusion,
# FDS_CONE_FORCE=1 fused) which carry no dual-arm tax.
#   usage: fused_ab.sh [rounds] [t] [iters] [scene] [armset]
set -u
WT=${WT:-/Users/gil-ad/work/rev-conevec}
ROUNDS=${1:-6}; POSE=${2:-1961}; ITERS=${3:-6}; SCENE=${4:-city}; SET=${5:-main}
OUT=${AB_OUT:-/tmp/fused_ab_$SCENE$POSE}
cd $WT/Runtime || exit 1
mkdir -p $OUT; rm -f $OUT/r*_*.txt

run() {  # $1 round  $2 label  $3 binary  $4 extra flags
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$3 \
    --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
    --deferred --texture_filter=1 --profiler=1 \
    --deferred_prof=1 --hw_prof --strict_flags ${=4} 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt
}

if [[ $SET == main ]]; then ARMS=(PAR OFF ON); else ARMS=(F2 F1); fi
print -r -- "# fused A/B scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS set=$SET"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  # rotate the arm order every round so no arm sits at a fixed thermal slot
  n=${#ARMS[@]}; ORD=()
  for i in $(seq 0 $((n-1))); do ORD+=(${ARMS[$(( (i + r) % n + 1 ))]}); done
  for a in $ORD; do
    case $a in
      PAR) run $r PAR DEMO_parent "" ;;
      OFF) run $r OFF DEMO_new "--no-vol_cone_fused" ;;
      ON)  run $r ON  DEMO_new "--vol_cone_fused" ;;
      F2)  run $r F2  DEMO_force2 "" ;;
      F1)  run $r F1  DEMO_force1 "" ;;
    esac
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done

python3 - "$OUT" <<'PY'
import sys, glob, re, os, collections
out = sys.argv[1]
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out, "r*_*.txt"))):
    arm = os.path.basename(f).split("_",1)[1].split(".")[0]
    for line in open(f):
        m = re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if not m: continue
        name = m.group(1).strip()
        if name not in ("renderFrame","cones","fastfog","DeferredLighting-call","gbuffer","TBR-render"): continue
        try: rows[name][arm].append((float(m.group(3)), float(m.group(7)), float(m.group(8)), float(m.group(9))))
        except ValueError: pass
order = ["PAR","OFF","ON","F2","F1"]
hdr = f"{'phase':<22}{'arm':<5}{'wall_min':>9}{'Ginstr/f':>10}{'Gcyc/f':>9}{'IPC':>7}{'n':>4}"
print(hdr); print("-"*len(hdr))
for name in ("cones","renderFrame","DeferredLighting-call","fastfog","gbuffer","TBR-render"):
    if name not in rows: continue
    agg={}
    for arm in order:
        v = rows[name].get(arm)
        if not v: continue
        agg[arm]=(min(x[0] for x in v), min(x[1] for x in v), min(x[2] for x in v), sum(x[3] for x in v)/len(v), len(v))
        w,gi,gc,ipc,n=agg[arm]
        print(f"{name:<22}{arm:<5}{w:>9.3f}{gi:>10.3f}{gc:>9.3f}{ipc:>7.3f}{n:>4}")
    for base,new,lab in (("PAR","ON","PAR->ON"),("PAR","OFF","PAR->OFF"),("OFF","ON","OFF->ON"),("F2","F1","F2->F1")):
        if base in agg and new in agg:
            a,b=agg[base],agg[new]
            print(f"{'  '+lab:<27}{b[0]-a[0]:>+9.3f}{b[1]-a[1]:>+10.3f}{b[2]-a[2]:>+9.3f}"
                  f"   ({100*(b[0]-a[0])/a[0]:+.1f}% wall, {100*(b[1]-a[1])/a[1]:+.1f}% instr, {100*(b[2]-a[2])/a[2]:+.1f}% cyc)")
    print()
PY
