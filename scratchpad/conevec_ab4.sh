#!/bin/zsh
# 4-arm interleaved bench of the cone-solve work, one asset tree, one pose.
# This is the script that produced the rejected-variant table in
# docs/HW_PROFILING.md section 9. It expects three binaries parked in Runtime/,
# which you have to rebuild — only the shipping arm lives in the tree:
#   ./DEMO_exact   default build             -> arms SCA (flag off) and VEC
#   ./DEMO_approx  -DFDS_CONE_SOLVE_APPROX=1 -> arm VEO (raw rcp/rsqrt, no NR)
#   ./DEMO_relax   the relaxed-FP spellings  -> arm VRX (min/max + fmsub; this
#                  one is not a switch, it is the hand edit described in the
#                  DeferredVolumetric.cpp note -- it moves the city pin by 3 px)
# The early-out arm is -DFDS_CONE_SOLVE_EARLYOUT=1 if you want to re-price it.
# Arm order rotates every round so a load ramp cannot sit on one arm.
# usage: conevec_ab4.sh [rounds] [t] [iters] [scene]
set -u
ROUNDS=${1:-6}; POSE=${2:-1961}; ITERS=${3:-6}; SCENE=${4:-city}
OUT=${AB_OUT:-/tmp/conevec_ab4}
mkdir -p $OUT; rm -f $OUT/r*_*.txt

typeset -A BIN FLAG
BIN=(SCA ./DEMO_exact VEC ./DEMO_exact VEO ./DEMO_approx VRX ./DEMO_relax)
FLAG=(SCA --no-vol_cone_solve_vec VEC --vol_cone_solve_vec VEO --vol_cone_solve_vec VRX --vol_cone_solve_vec)
ARMS=(SCA VEC VEO VRX)

run() {
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $BIN[$2] \
    --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
    --deferred --texture_filter=1 --profiler=1 \
    --deferred_prof=1 --hw_prof --strict_flags $FLAG[$2] 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt
}

print -r -- "# conevec 4-arm  scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  n=$(( (r-1) % 4 ))
  for i in $(seq 0 3); do
    idx=$(( (n + i) % 4 + 1 ))
    run $r $ARMS[$idx]
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done

python3 - "$OUT" <<'PY'
import sys, glob, re, os, collections
out = sys.argv[1]
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out, "r*_*.txt"))):
    arm = os.path.basename(f).split("_")[1].split(".")[0]
    for line in open(f):
        m = re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if not m: continue
        name = m.group(1).strip()
        if name not in ("renderFrame", "cones"): continue
        try:
            rows[name][arm].append((float(m.group(3)), float(m.group(7)),
                                    float(m.group(8)), float(m.group(9))))
        except ValueError:
            pass
hdr = f"{'phase':<13}{'arm':<5}{'wall_min':>9}{'Ginstr/f':>10}{'Gcyc/f':>9}{'IPC':>7}{'n':>4}{'vs SCA wall':>13}{'vs SCA instr':>14}"
print(hdr); print("-"*len(hdr))
for name in ("cones", "renderFrame"):
    base = rows[name].get("SCA")
    bw = min(x[0] for x in base); bi = min(x[1] for x in base)
    for arm in ("SCA", "VEC", "VEO", "VRX"):
        v = rows[name].get(arm)
        if not v: continue
        w = min(x[0] for x in v); gi = min(x[1] for x in v)
        gc = min(x[2] for x in v); ipc = sum(x[3] for x in v)/len(v)
        print(f"{name:<13}{arm:<5}{w:>9.3f}{gi:>10.3f}{gc:>9.3f}{ipc:>7.3f}{len(v):>4}"
              f"{100*(w-bw)/bw:>+12.1f}%{100*(gi-bi)/bi:>+13.1f}%")
    print()
PY
