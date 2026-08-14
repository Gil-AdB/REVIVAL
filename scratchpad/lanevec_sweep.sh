#!/bin/zsh
# City t-sweep of the cone pass, scalar per-lane arm vs 8-wide, interleaved per
# pose so a load ramp cannot land on one arm. Same poses as the a16567b sweep.
# usage: conevec_sweep.sh [iters] [rounds]
set -u
ITERS=${1:-6}; ROUNDS=${2:-2}
OUT=${SW_OUT:-/tmp/conevec_sweep}
BIN=${BIN:-./DEMO}
mkdir -p $OUT; rm -f $OUT/*.txt
POSES=(400 900 1400 1961 2400)

run() {  # $1 pose  $2 round  $3 arm  $4 flag
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $BIN \
    --bench=scene@scene=city,t=$1,iters=$ITERS \
    --deferred --texture_filter=1 --profiler=1 \
    --deferred_prof=1 --hw_prof --strict_flags $4 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/t$1_r$2_$3.txt
}

print -r -- "# city t-sweep, iters=$ITERS rounds=$ROUNDS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for p in $POSES; do
  for r in $(seq 1 $ROUNDS); do
    if (( r % 2 )); then run $p $r SCA --no-vol_cone_lane_vec; run $p $r VEC --vol_cone_lane_vec
    else                 run $p $r VEC --vol_cone_lane_vec; run $p $r SCA --no-vol_cone_lane_vec; fi
  done
  print -r -- "# t=$p done (load $(uptime | sed 's/.*averages: //'))"
done

python3 - "$OUT" <<'PY'
import sys, glob, re, os, collections
out = sys.argv[1]
d = collections.defaultdict(dict)
acc = collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out, "t*_r*_*.txt"))):
    b = os.path.basename(f)[:-4].split("_")
    pose, arm = int(b[0][1:]), b[2]
    for line in open(f):
        m = re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if not m: continue
        name = m.group(1).strip()
        if name not in ("renderFrame", "cones"): continue
        try:
            acc[(pose, name)][arm].append((float(m.group(3)), float(m.group(7)), float(m.group(9))))
        except ValueError: pass
print(f"{'t':>6} | {'cones Ginstr/f':>26} | {'cones wall_min ms':>26} | {'cones share of frame':>22}")
print(f"{'':>6} | {'SCA':>8}{'VEC':>9}{'delta':>9} | {'SCA':>8}{'VEC':>9}{'delta':>9} | {'SCA':>10}{'VEC':>11}")
print("-"*92)
for pose in sorted({p for p, n in acc}):
    c, rf = acc[(pose, "cones")], acc[(pose, "renderFrame")]
    if not c or not rf: continue
    gs, gv = min(x[1] for x in c["SCA"]), min(x[1] for x in c["VEC"])
    ws, wv = min(x[0] for x in c["SCA"]), min(x[0] for x in c["VEC"])
    fs, fv = min(x[1] for x in rf["SCA"]), min(x[1] for x in rf["VEC"])
    print(f"{pose:>6} | {gs:>8.3f}{gv:>9.3f}{100*(gv-gs)/gs:>+8.1f}% | "
          f"{ws:>8.1f}{wv:>9.1f}{100*(wv-ws)/ws:>+8.1f}% | {100*gs/fs:>9.0f}%{100*gv/fv:>10.0f}%")
PY
