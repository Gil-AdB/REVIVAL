#!/bin/zsh
# Interleaved ABBA A/B of the 8-wide cone solve (--vol_cone_solve_vec) against
# the scalar arm (--no-vol_cone_solve_vec), one binary, one asset tree.
# Reports per-arm min-over-rounds of cones wall_min / Ginstr/f / Gcyc/f / IPC
# and the same for renderFrame.  usage: conevec_ab.sh [rounds] [t] [iters]
set -u
ROUNDS=${1:-6}; POSE=${2:-1961}; ITERS=${3:-6}; SCENE=${4:-city}
OUT=${AB_OUT:-/tmp/conevec_ab}
BIN=${BIN:-./DEMO}
mkdir -p $OUT; rm -f $OUT/r*_*.txt

run() {  # $1 round  $2 arm label  $3 extra flag
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $BIN \
    --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
    --deferred --texture_filter=1 --profiler=1 \
    --deferred_prof=1 --hw_prof --strict_flags $3 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt
}

print -r -- "# conevec A/B scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  if (( r % 2 )); then run $r VEC --vol_cone_solve_vec; run $r SCA --no-vol_cone_solve_vec
  else                 run $r SCA --no-vol_cone_solve_vec; run $r VEC --vol_cone_solve_vec; fi
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
        if name not in ("renderFrame", "cones", "cones-call", "fastfog",
                        "DeferredLighting-call", "gbuffer", "TBR-render"): continue
        try:
            rows[name][arm].append((float(m.group(3)), float(m.group(7)),
                                    float(m.group(8)), float(m.group(9))))
        except ValueError:
            pass
hdr = f"{'phase':<24}{'arm':<5}{'wall_min':>9}{'Ginstr/f':>10}{'Gcyc/f':>9}{'IPC':>7}{'n':>4}"
print(hdr); print("-"*len(hdr))
for name in ("renderFrame", "cones", "DeferredLighting-call", "fastfog", "gbuffer", "TBR-render"):
    if name not in rows: continue
    for arm in ("SCA", "VEC"):
        v = rows[name].get(arm)
        if not v: continue
        w = min(x[0] for x in v); gi = min(x[1] for x in v)
        gc = min(x[2] for x in v); ipc = sum(x[3] for x in v)/len(v)
        print(f"{name:<24}{arm:<5}{w:>9.3f}{gi:>10.3f}{gc:>9.3f}{ipc:>7.3f}{len(v):>4}")
    a = rows[name].get("SCA"); b = rows[name].get("VEC")
    if a and b:
        wa, wb = min(x[0] for x in a), min(x[0] for x in b)
        ia, ib = min(x[1] for x in a), min(x[1] for x in b)
        ca, cb = min(x[2] for x in a), min(x[2] for x in b)
        print(f"{'  -> delta':<24}{'':<5}{wb-wa:>+9.3f}{ib-ia:>+10.3f}{cb-ca:>+9.3f}"
              f"   ({100*(wb-wa)/wa:+.1f}% wall, {100*(ib-ia)/ia:+.1f}% instr, {100*(cb-ca)/ca:+.1f}% cyc)")
PY
