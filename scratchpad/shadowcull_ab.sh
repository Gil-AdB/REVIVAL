#!/bin/zsh
# Interleaved N-arm A/B for the shadow-cull work (docs/HW_PROFILING.md §5 shape,
# generalised past two arms). Each arm is "<binary>|<extra flags>", so a run can
# mix a parent-commit binary with flag arms of the instrumented one — which is
# the three-arm protocol this campaign uses: parent / feature-OFF / feature-ON.
# Arm order rotates every round so a load ramp cannot land on one arm; the
# reported number is min-over-rounds per phase.
#
#   usage: shadowcull_ab.sh <rounds> <scene> <pose> <iters> "<bin>|<flags>" ...
#   env:   SC_OUT (raw [DPROF] dumps), SC_RES ("xres=1512,yres=848" or empty)
#   run from Runtime/.
set -u
ROUNDS=${1:?rounds}; SCENE=${2:?scene}; POSE=${3:?pose}; ITERS=${4:?iters}
shift 4
ARMS=("$@")
OUT=${SC_OUT:-/tmp/shadowcull_ab}
RES=${SC_RES:-}
mkdir -p $OUT; rm -f $OUT/r*_a*.txt

BASE=${SC_BASE:---deferred --texture_filter=1 --profiler=1 --deferred_prof=1 --hw_prof --strict_flags}
BENCH="--bench=scene@scene=$SCENE,t=$POSE,iters=$ITERS"
[ -n "$RES" ] && BENCH="$BENCH,$RES"

print -r -- "# shadowcull A/B  scene=$SCENE t=$POSE iters=$ITERS rounds=$ROUNDS arms=${#ARMS[@]} res='${RES:-cfg}'"
print -r -- "# base: $BASE"
for k in {1..${#ARMS[@]}}; do print -r -- "# arm$k: ${ARMS[$k]}"; done
print -r -- "# load at start: $(uptime | sed 's/.*averages: //')"

for r in $(seq 1 $ROUNDS); do
  for kk in {1..${#ARMS[@]}}; do
    k=$(( (kk - 1 + r - 1) % ${#ARMS[@]} + 1 ))
    spec=${ARMS[$k]}
    bin=${spec%%|*}
    flags=${spec#*|}
    [ "$flags" = "$spec" ] && flags=""
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$bin" \
      ${=BENCH} ${=BASE} ${=flags} 2>&1 \
      | grep -E '^\[DPROF\]' > $OUT/r${r}_a${k}.txt
  done
  print -r -- "# round $r done  (load $(uptime | sed 's/.*averages: //'))"
done

python3 - $OUT ${#ARMS[@]} <<'PY'
import sys, os, re, collections
out, narms = sys.argv[1], int(sys.argv[2])
want = ["renderFrame", "DeferredLighting-call", "lighting-w1", "lighting-w2",
        "gbuffer", "tile-cull", "depth-bounds"]
res = collections.defaultdict(lambda: collections.defaultdict(list))
for fn in sorted(os.listdir(out)):
    m = re.match(r"r(\d+)_a(\d+)\.txt$", fn)
    if not m: continue
    arm = int(m.group(2))
    for line in open(os.path.join(out, fn)):
        if not line.startswith("[DPROF]"): continue
        f = line[len("[DPROF]"):].split()
        if not f or f[0] not in want: continue
        # name calls wall_min wall_avg thrsum effPar | Ginstr Gcyc IPC
        try:
            wall = float(f[2])
        except (IndexError, ValueError):
            continue
        gi = gc = ipc = "-"
        if "|" in f:
            p = f.index("|")
            if len(f) > p + 3: gi, gc, ipc = f[p+1], f[p+2], f[p+3]
        res[f[0]][arm].append((wall, gi, gc, ipc))
print(f"{'phase':<22} " + " ".join(f"{'arm'+str(a):>24}" for a in range(1, narms+1)))
for w in want:
    if w not in res: continue
    row = f"{w:<22} "; base = None
    for a in range(1, narms+1):
        vs = res[w].get(a, [])
        if not vs: row += f"{'-':>24} "; continue
        mn = min(v[0] for v in vs)
        if base is None: base = mn
        d = "" if a == 1 else f" {mn-base:+.3f}"
        row += f"{mn:>10.3f}{d:>8}  "
    print(row)
print()
print("Ginstr/f and IPC (from each arm's min-wall run):")
for w in want:
    if w not in res: continue
    line = f"{w:<22} "
    for a in range(1, narms+1):
        vs = res[w].get(a, [])
        if not vs: line += f"{'-':>24} "; continue
        b = min(vs, key=lambda v: v[0])
        line += f" Gi={b[1]:>6} IPC={b[3]:>6} "
    print(line)
print()
print("per-round wall_min spread (renderFrame):")
for a in range(1, narms+1):
    vs = sorted(v[0] for v in res.get("renderFrame", {}).get(a, []))
    print(f"  arm{a}: " + " ".join(f"{v:.2f}" for v in vs))
PY
