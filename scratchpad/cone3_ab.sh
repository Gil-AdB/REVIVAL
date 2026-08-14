#!/bin/zsh
# GENERIC three-arm interleaved round-robin A/B for the cone campaign (round 4).
# Same protocol round 2 established and round 3 re-proved: a one-binary flag-OFF
# baseline OVERSTATES the gain, because merely compiling the new arm in perturbs
# the old one's register allocation. So the baseline is always a PARENT-COMMIT
# BINARY, and the new binary's OFF arm is reported beside it as the dual-arm tax.
#
# Park both binaries in <worktree>/Runtime/ and codesign -f -s - each (DEMO
# resolves assets from its own directory; macOS SIGKILLs an unsigned copy):
#   DEMO_parent  : the parent commit, plain build
#   DEMO_new     : the candidate, plain build
#
#   usage:  FLAG=vol_cone_solve_w4 cone3_ab.sh [rounds] [t] [iters] [scene]
set -u
WT=${WT:-/Users/gil-ad/work/rev-conevec}
FLAG=${FLAG:?set FLAG to the feature-flag name under test}
PAR=${PAR:-DEMO_parent}
NEW=${NEW:-DEMO_new}
ROUNDS=${1:-6}; POSE=${2:-1961}; ITERS=${3:-6}; SCENE=${4:-city}
OUT=${AB_OUT:-/tmp/cone3_${FLAG}_${SCENE}$POSE}
cd $WT/Runtime || exit 1
mkdir -p $OUT; rm -f $OUT/r*_*.txt

run() {  # $1 round  $2 label  $3 binary  $4 extra flags
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$3 \
    --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
    --deferred --texture_filter=1 --profiler=1 \
    --deferred_prof=1 --hw_prof --strict_flags ${=4} 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt
}

ARMS=(PAR OFF ON)
print -r -- "# cone3 A/B flag=$FLAG scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  n=${#ARMS[@]}; ORD=()
  for i in $(seq 0 $((n-1))); do ORD+=(${ARMS[$(( (i + r) % n + 1 ))]}); done
  for a in $ORD; do
    case $a in
      PAR) run $r PAR $PAR "" ;;
      OFF) run $r OFF $NEW "--no-$FLAG" ;;
      ON)  run $r ON  $NEW "--$FLAG" ;;
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
        if name not in ("renderFrame","cones","DeferredLighting-call","fastfog"): continue
        try: rows[name][arm].append((float(m.group(3)), float(m.group(7)), float(m.group(8)), float(m.group(9))))
        except ValueError: pass
order = ["PAR","OFF","ON"]
hdr = f"{'phase':<22}{'arm':<5}{'wall_min':>9}{'Ginstr/f':>10}{'Gcyc/f':>9}{'IPC':>7}{'n':>4}"
print(hdr); print("-"*len(hdr))
for name in ("cones","renderFrame","DeferredLighting-call","fastfog"):
    if name not in rows: continue
    agg={}
    for arm in order:
        v = rows[name].get(arm)
        if not v: continue
        agg[arm]=(min(x[0] for x in v), min(x[1] for x in v), min(x[2] for x in v), sum(x[3] for x in v)/len(v), len(v))
        w,gi,gc,ipc,n=agg[arm]
        print(f"{name:<22}{arm:<5}{w:>9.3f}{gi:>10.3f}{gc:>9.3f}{ipc:>7.3f}{n:>4}")
    for base,new,lab in (("PAR","ON","PAR->ON"),("PAR","OFF","PAR->OFF"),("OFF","ON","OFF->ON")):
        if base in agg and new in agg:
            a,b=agg[base],agg[new]
            print(f"{'  '+lab:<27}{b[0]-a[0]:>+9.3f}{b[1]-a[1]:>+10.3f}{b[2]-a[2]:>+9.3f}"
                  f"   ({100*(b[0]-a[0])/a[0]:+.1f}% wall, {100*(b[1]-a[1])/a[1]:+.1f}% instr, {100*(b[2]-a[2])/a[2]:+.1f}% cyc)")
    print()
PY
