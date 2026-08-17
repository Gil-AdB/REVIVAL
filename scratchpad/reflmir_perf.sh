#!/bin/zsh
# reflmir_perf.sh — price --refl_correct.
#
# Two halves to price, and they live in DIFFERENT places:
#   (a) the TN/TTangent write, inside Reflected_Transform — OUTSIDE renderFrame,
#       so no [DPROF] row can see it. Measured by a TWO-POINT wall clock over the
#       snapshot harness: t(NHI poses) - t(NLO poses) cancels process init exactly,
#       leaving (NHI-NLO) frames of real work.
#   (b) the light mirroring, inside Render_DeferredLighting — visible as the
#       [DPROF] `light-list` row.
# Three arms, order-rotated per round, min-of-rounds (the floor is the estimator;
# the mean is contaminated by whatever else the machine did).
#   par  = parent binary
#   off  = child with --no-refl_correct  (the control: proves the child's floor
#          equals the parent's when the feature is off)
#   on   = child, shipping default
set -u
zmodload zsh/datetime
setopt null_glob
cd "${0:A:h}/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ROUNDS=${ROUNDS:-9}
NLO=${NLO:-8}
NHI=${NHI:-48}
POSE=${POSE:-800}
OUT=${OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/reflmir_perf}
mkdir -p $OUT
typeset -a NAMES BINS FLAGS
NAMES=(par off on)
BINS=(DEMO_base DEMO_fix DEMO_fix)
FLAGS=("" "--no-refl_correct" "")

tlist() { python3 -c "print(','.join(['$1']*$2))"; }
TLO=$(tlist $POSE $NLO); THI=$(tlist $POSE $NHI)

print -r -- "# reflmir perf  pose=$POSE  NLO=$NLO NHI=$NHI rounds=$ROUNDS  load: $(uptime | sed 's/.*averages: //')"
rm -f $OUT/wall_*.txt $OUT/dprof_*.txt 2>/dev/null
N=${#NAMES}
for r in $(seq 1 $ROUNDS); do
  for k in $(seq 1 $N); do
    idx=$(( (k + r - 2) % N + 1 ))
    nm=${NAMES[$idx]}; bn=${BINS[$idx]}; fl=${FLAGS[$idx]}
    for tag t in lo $TLO hi $THI; do
      d=$(mktemp -d)
      s=$EPOCHREALTIME
      ./$bn --snapshot=chase@t=$t --out=$d --deferred ${=fl} >/dev/null 2>&1
      e=$EPOCHREALTIME
      print -r -- "$(python3 -c "print(f'{($e-$s)*1000:.1f}')")" >> $OUT/wall_${nm}_${tag}.txt
      rm -rf $d
    done
    # [DPROF] pass for the light-list row (hi count only)
    d=$(mktemp -d)
    ./$bn --snapshot=chase@t=$THI --out=$d --deferred ${=fl} --deferred_prof=1 --hw_prof 2>&1 \
      | grep -E '^\[DPROF\]' >> $OUT/dprof_${nm}.txt
    rm -rf $d
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done

python3 - "$OUT" "$NLO" "$NHI" <<'PY'
import sys, glob, os, re, collections
out, nlo, nhi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
w = {}
for f in sorted(glob.glob(os.path.join(out, "wall_*.txt"))):
    b = os.path.basename(f)[:-4].split("_")
    w.setdefault(b[1], {})[b[2]] = [float(x) for x in open(f) if x.strip()]
print("\n%-6s %10s %10s %14s %10s" % ("arm", "lo_min", "hi_min", "ms/frame", "floor%"))
print("-"*56)
base = None
for nm in ("par", "off", "on"):
    if nm not in w: continue
    lo, hi = min(w[nm]["lo"]), min(w[nm]["hi"])
    per = (hi - lo) / (nhi - nlo)
    lo2 = sorted(w[nm]["lo"])[1] if len(w[nm]["lo"]) > 1 else lo
    hi2 = sorted(w[nm]["hi"])[1] if len(w[nm]["hi"]) > 1 else hi
    floor = 100.0 * (((hi2-lo2)/(nhi-nlo)) - per) / per if per else 0.0
    if nm == "par": base = per
    d = "" if base is None or nm == "par" else "   (%+.2f%% vs par)" % (100.0*(per-base)/base)
    print("%-6s %10.1f %10.1f %14.3f %9.2f%%%s" % (nm, lo, hi, per, floor, d))

rows = collections.defaultdict(lambda: collections.defaultdict(list))
pat = re.compile(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)")
for f in sorted(glob.glob(os.path.join(out, "dprof_*.txt"))):
    nm = os.path.basename(f)[6:-4]
    for line in open(f):
        m = pat.match(line)
        if not m: continue
        name = m.group(1).strip()
        if name in ("renderFrame", "light-list", "DeferredLighting-call", "gbuffer", "tile-cull"):
            rows[name][nm].append(float(m.group(4)))
print("\n%-24s %10s %10s %10s   %s" % ("[DPROF] wall_avg ms/f", "par", "off", "on", "on vs par"))
print("-"*70)
for name in ("renderFrame", "gbuffer", "DeferredLighting-call", "light-list", "tile-cull"):
    if name not in rows: continue
    v = {k: min(x) for k, x in rows[name].items()}
    if "par" not in v or "on" not in v: continue
    dd = 100.0*(v["on"]-v["par"])/v["par"] if v["par"] else 0.0
    print("%-24s %10.3f %10.3f %10.3f   %+.2f%%" % (name, v.get("par",0), v.get("off",0), v.get("on",0), dd))
PY
