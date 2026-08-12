#!/bin/zsh
# Interleaved A/B of --vol_cone_solve_vec on GREETS (narrow disco-beam cones,
# the segmented-hybrid branch) at the pin pose. City's headlights are all wide
# cones on the cheapest branch; greets is the other shape, so it gets its own
# measurement rather than an assumption. Run from the worktree's Runtime/.
set -u
ROUNDS=${1:-6}; ITERS=${2:-4}
OUT=${AB_OUT:-/tmp/greets_ab}
mkdir -p $OUT; rm -f $OUT/r*_*.txt
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"

runarm() {
  local flag=--no-vol_cone_solve_vec
  [[ $2 == VEC ]] && flag=--vol_cone_solve_vec
  ./DEMO --bench=scene@scene=greets,t=1588,iters=$ITERS \
    --deferred --hdr --profiler=1 --deferred_prof=1 --hw_prof $flag 2>&1 \
  | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt
}

print -r -- "# greets cone-solve A/B  iters=$ITERS rounds=$ROUNDS"
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"
for r in $(seq 1 $ROUNDS); do
  if (( r % 2 )); then runarm $r VEC; runarm $r SCA
  else                 runarm $r SCA; runarm $r VEC; fi
done
print -r -- "# load at end: $(uptime | sed 's/.*averages: //')"

python3 - "$OUT" <<'PY'
import sys, glob, re, os, collections
out = sys.argv[1]
acc = collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out, "r*_*.txt"))):
    arm = os.path.basename(f)[:-4].split("_")[1]
    for line in open(f):
        m = re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if not m: continue
        n = m.group(1).strip()
        if n not in ("renderFrame", "cones"): continue
        try: acc[n][arm].append((float(m.group(3)), float(m.group(7)), float(m.group(8))))
        except ValueError: pass
for n in ("cones", "renderFrame"):
    s, v = acc[n].get("SCA"), acc[n].get("VEC")
    if not s or not v: continue
    ws, wv = min(x[0] for x in s), min(x[0] for x in v)
    gs, gv = min(x[1] for x in s), min(x[1] for x in v)
    cs, cv = min(x[2] for x in s), min(x[2] for x in v)
    print(f"{n:<12} wall_min {ws:7.3f} -> {wv:7.3f} ({100*(wv-ws)/ws:+5.1f}%)   "
          f"Ginstr {gs:6.3f} -> {gv:6.3f} ({100*(gv-gs)/gs:+5.1f}%)   "
          f"Gcyc {cs:6.3f} -> {cv:6.3f} ({100*(cv-cs)/cs:+5.1f}%)  n={len(s)}")
PY
