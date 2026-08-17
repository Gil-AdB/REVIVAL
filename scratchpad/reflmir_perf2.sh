#!/bin/zsh
# reflmir_perf2.sh — the RESOLVING price run for --refl_correct.
#
# Round 1 said +0.16 %, round 2 said +2.45 %. Two runs that disagree by 15x are
# not a measurement, so this replaces the estimator on both scenes:
#   * city  — has a real --bench arm: mean ms/iter over `iters` frames with the
#     warmup excluded by the harness itself. This is the low-noise instrument.
#   * chase — no --bench arm (BENCH says so), so keep the two-point wall clock
#     but push NHI to 160 frames (~8 s of frames against ~0.6 s of init), which
#     shrinks init variance to a few per mille of the difference.
# The arm that MEANS anything is `on` vs `off`: SAME BINARY, flag flipped, so
# LTO layout is held fixed. `par` is carried only to show the between-binary
# floor, which round 2 measured at 1.8 % (off came out FASTER than par doing
# strictly the same work).
set -u
zmodload zsh/datetime
setopt null_glob
cd "${0:A:h}/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ROUNDS=${ROUNDS:-11}
OUT=${OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/reflmir_perf3}
mkdir -p $OUT
rm -f $OUT/*.txt 2>/dev/null
typeset -a NAMES BINS FLAGS
NAMES=(par off on)
BINS=(DEMO_base DEMO_fix DEMO_fix)
FLAGS=("" "--no-refl_correct" "")

NLO=${NLO:-16}; NHI=${NHI:-160}; POSE=${POSE:-800}
tlist() { python3 -c "print(','.join(['$1']*$2))"; }
TLO=$(tlist $POSE $NLO); THI=$(tlist $POSE $NHI)
CITY_ITERS=${CITY_ITERS:-40}

print -r -- "# reflmir perf2 rounds=$ROUNDS chase(NLO=$NLO NHI=$NHI pose=$POSE) city(bench iters=$CITY_ITERS) load: $(uptime | sed 's/.*averages: //')"
N=${#NAMES}
for r in $(seq 1 $ROUNDS); do
  for k in $(seq 1 $N); do
    idx=$(( (k + r - 2) % N + 1 ))
    nm=${NAMES[$idx]}; bn=${BINS[$idx]}; fl=${FLAGS[$idx]}
    # --- city: the low-noise instrument
    ./$bn --bench=scene@scene=city,t=1961,iters=$CITY_ITERS --deferred --profiler=0 ${=fl} 2>&1 \
      | grep -E '^\[BENCH\].*(mean|ms)' >> $OUT/city_${nm}.txt
    # --- chase: two-point wall clock
    for tag t in lo $TLO hi $THI; do
      d=$(mktemp -d); s=$EPOCHREALTIME
      ./$bn --snapshot=chase@t=$t --out=$d --deferred ${=fl} >/dev/null 2>&1
      e=$EPOCHREALTIME
      print -r -- "$(python3 -c "print(f'{($e-$s)*1000:.1f}')")" >> $OUT/chase_${nm}_${tag}.txt
      rm -rf $d
    done
  done
  print -r -- "# round $r done (load $(uptime | sed 's/.*averages: //'))"
done

print -r -- "\n=== raw city bench lines (first arm) ==="
head -3 $OUT/city_par.txt

python3 - "$OUT" "$NLO" "$NHI" <<'PY'
import sys, glob, os, re
out, nlo, nhi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

# ---- city: pull every float that looks like ms/iter out of the BENCH lines
num = re.compile(r"([\d.]+)\s*ms")
city = {}
for f in sorted(glob.glob(os.path.join(out, "city_*.txt"))):
    nm = os.path.basename(f)[5:-4]
    vals = []
    for line in open(f):
        m = num.findall(line)
        if m: vals.append(float(m[0]))
    if vals: city[nm] = vals
if city:
    print("\n%-6s %10s %10s %10s %9s   %s" % ("arm", "n", "min", "median", "floor%", "vs off"))
    print("-"*66)
    ref = min(city["off"]) if "off" in city else None
    for nm in ("par", "off", "on"):
        if nm not in city: continue
        v = sorted(city[nm]); mn = v[0]; md = v[len(v)//2]
        floor = 100.0*(v[1]-v[0])/v[0] if len(v) > 1 else 0.0
        d = "" if ref is None else "  %+.2f%%" % (100.0*(mn-ref)/ref)
        print("%-6s %10d %10.3f %10.3f %8.2f%%%s" % (nm, len(v), mn, md, floor, d))

# ---- chase two-point
w = {}
for f in sorted(glob.glob(os.path.join(out, "chase_*.txt"))):
    b = os.path.basename(f)[:-4].split("_")
    w.setdefault(b[1], {})[b[2]] = [float(x) for x in open(f) if x.strip()]
if w:
    print("\n%-6s %10s %10s %14s %9s   %s" % ("arm", "lo_min", "hi_min", "ms/frame", "floor%", "vs off"))
    print("-"*66)
    per = {}
    for nm in ("par", "off", "on"):
        if nm not in w: continue
        lo, hi = min(w[nm]["lo"]), min(w[nm]["hi"])
        per[nm] = (hi - lo) / (nhi - nlo)
    for nm in ("par", "off", "on"):
        if nm not in per: continue
        lo_s, hi_s = sorted(w[nm]["lo"]), sorted(w[nm]["hi"])
        p2 = (hi_s[1]-lo_s[1])/(nhi-nlo) if len(hi_s) > 1 else per[nm]
        floor = 100.0*(p2-per[nm])/per[nm]
        d = "" if "off" not in per else "  %+.2f%%" % (100.0*(per[nm]-per["off"])/per["off"])
        print("%-6s %10.1f %10.1f %14.3f %8.2f%%%s" % (nm, min(w[nm]["lo"]), min(w[nm]["hi"]), per[nm], floor, d))
PY
