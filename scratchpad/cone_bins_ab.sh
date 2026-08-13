#!/bin/zsh
# N-binary interleaved round-robin bench. Arms are BINARIES parked in Runtime/,
# no flags — for pricing single-arm control builds (FDS_CONE_FORCE / _HOTONLY /
# _W4FORCE), which is how round 2's method finding says a spelling has to be
# priced: a flag A/B inside one binary measures against a baseline the new arm
# has already degraded.
#   usage: BINS="DEMO_c8 DEMO_c4u DEMO_c4r" cone_bins_ab.sh [rounds] [t] [iters] [scene]
set -u
WT=${WT:-/Users/gil-ad/work/rev-conevec}
BINS=${BINS:?set BINS to a space-separated list of binaries in Runtime/}
ROUNDS=${1:-6}; POSE=${2:-1961}; ITERS=${3:-6}; SCENE=${4:-city}
OUT=${AB_OUT:-/tmp/cone_bins_${SCENE}$POSE}
cd $WT/Runtime || exit 1
mkdir -p $OUT; rm -f $OUT/r*_*.txt
ARMS=(${=BINS})
print -r -- "# cone bins A/B bins='$BINS' scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  n=${#ARMS[@]}; ORD=()
  for i in $(seq 0 $((n-1))); do ORD+=(${ARMS[$(( (i + r) % n + 1 ))]}); done
  for a in $ORD; do
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$a \
      --bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS \
      --deferred --texture_filter=1 --profiler=1 \
      --deferred_prof=1 --hw_prof --strict_flags 2>&1 \
    | grep -E '^\[DPROF\]' > $OUT/r${r}_${a}.txt
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done
python3 - "$OUT" "$BINS" <<'PY'
import sys, glob, re, os, collections
out, bins = sys.argv[1], sys.argv[2].split()
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out, "r*_*.txt"))):
    arm = os.path.basename(f).split("_",1)[1][:-4]
    for line in open(f):
        m = re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if not m: continue
        name = m.group(1).strip()
        if name not in ("renderFrame","cones"): continue
        try: rows[name][arm].append((float(m.group(3)), float(m.group(7)), float(m.group(8)), float(m.group(9))))
        except ValueError: pass
hdr = f"{'phase':<12}{'arm':<14}{'wall_min':>9}{'Ginstr/f':>10}{'Gcyc/f':>9}{'IPC':>7}{'n':>4}{'vs arm1 cyc':>13}{'vs arm1 wall':>14}"
print(hdr); print("-"*len(hdr))
for name in ("cones","renderFrame"):
    if name not in rows: continue
    agg={}
    for arm in bins:
        v = rows[name].get(arm)
        if not v: continue
        agg[arm]=(min(x[0] for x in v), min(x[1] for x in v), min(x[2] for x in v), sum(x[3] for x in v)/len(v), len(v))
    b0 = agg.get(bins[0])
    for arm in bins:
        if arm not in agg: continue
        w,gi,gc,ipc,n = agg[arm]
        dc = 100*(gc-b0[2])/b0[2]; dw = 100*(w-b0[0])/b0[0]
        print(f"{name:<12}{arm:<14}{w:>9.3f}{gi:>10.3f}{gc:>9.3f}{ipc:>7.3f}{n:>4}{dc:>+12.1f}%{dw:>+13.1f}%")
    print()
PY
