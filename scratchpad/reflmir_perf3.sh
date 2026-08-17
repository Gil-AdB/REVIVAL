#!/bin/zsh
# reflmir_perf3.sh — the price of --refl_correct, 2026-08-17 (rebased pair).
#
# Two halves to price, in DIFFERENT places:
#   (a) the TN/TTangent write inside Reflected_Transform — OUTSIDE renderFrame,
#       so no [DPROF] row sees it. Two-point wall clock: t(NHI) - t(NLO) cancels
#       process init exactly, leaving (NHI-NLO) frames of real work.
#   (b) the light mirroring inside Render_DeferredLighting — one pass over ~40
#       lights per reflected frame.
# THE ARM THAT MEANS ANYTHING IS `on` vs `off`: SAME BINARY, flag flipped, so
# LTO layout is held fixed. `par` is carried only to show the between-binary
# floor (16y measured that floor at 1.8 %, i.e. bigger than the effect).
# min-of-rounds is the estimator; the mean is contaminated by the machine.
set -u
zmodload zsh/datetime
setopt null_glob
cd "${0:A:h}/../Runtime" || exit 2
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ROUNDS=${ROUNDS:-9}
OUT=${OUT:-/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/reflmir_perf3}
mkdir -p $OUT; rm -f $OUT/*.txt 2>/dev/null
typeset -a NAMES BINS FLAGS
NAMES=(par off on)
BINS=(DEMO_par DEMO_new2 DEMO_new2)
FLAGS=("" "--no-refl_correct" "")

NLO=${NLO:-16}; NHI=${NHI:-160}; POSE=${POSE:-800}
tlist() { python3 -c "print(','.join(['$1']*$2))"; }
TLO=$(tlist $POSE $NLO); THI=$(tlist $POSE $NHI)
CITY_ITERS=${CITY_ITERS:-40}

print -r -- "# reflmir perf3 rounds=$ROUNDS chase(NLO=$NLO NHI=$NHI pose=$POSE) city(bench iters=$CITY_ITERS) load: $(uptime | sed 's/.*averages: //')"
N=${#NAMES}
for r in $(seq 1 $ROUNDS); do
  for k in $(seq 1 $N); do
    idx=$(( (k + r - 2) % N + 1 ))
    nm=${NAMES[$idx]}; bn=${BINS[$idx]}; fl=${FLAGS[$idx]}
    ./$bn --bench=scene@scene=city,t=1961,iters=$CITY_ITERS --deferred --profiler=0 ${=fl} 2>&1 \
      | grep -E 'mean=' >> $OUT/city_${nm}.txt
    for tag t in lo $TLO hi $THI; do
      d=$(mktemp -d); s=$EPOCHREALTIME
      ./$bn --snapshot=chase@t=$t --out=$d --deferred ${=fl} >/dev/null 2>&1
      e=$EPOCHREALTIME
      print -r -- "$(python3 -c "print(f'{($e-$s)*1000:.1f}')")" >> $OUT/chase_${nm}_${tag}.txt
      rm -rf $d
    done
  done
  print -r -- "# round $r done"
done

python3 - "$OUT" "$NLO" "$NHI" <<'PY'
import sys, glob, os, re
out, nlo, nhi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
num = re.compile(r"mean=([\d.]+)")
city = {}
for f in sorted(glob.glob(os.path.join(out, "city_*.txt"))):
    nm = os.path.basename(f)[5:-4]
    vals = [float(m.group(1)) for line in open(f) for m in [num.search(line)] if m]
    if vals: city[nm] = vals
if city:
    print("\n=== city --bench mean ms/iter (t=1961, %d iters) ===" % 40)
    print("%-5s %5s %9s %9s %8s   %s" % ("arm", "n", "min", "median", "floor%", "vs off"))
    ref = min(city["off"]) if "off" in city else None
    for nm in ("par", "off", "on"):
        if nm not in city: continue
        v = sorted(city[nm]); mn, md = v[0], v[len(v)//2]
        floor = 100.0*(v[1]-v[0])/v[0] if len(v) > 1 else 0.0
        d = "" if ref is None else "  %+.2f%%" % (100.0*(mn-ref)/ref)
        print("%-5s %5d %9.3f %9.3f %7.2f%%%s" % (nm, len(v), mn, md, floor, d))
w = {}
for f in sorted(glob.glob(os.path.join(out, "chase_*.txt"))):
    b = os.path.basename(f)[:-4].split("_")
    w.setdefault(b[1], {})[b[2]] = [float(x) for x in open(f) if x.strip()]
if w:
    print("\n=== chase two-point wall clock (pose %d, %d->%d frames) ===" % (800, nlo, nhi))
    print("%-5s %9s %9s %11s %8s   %s" % ("arm", "lo_min", "hi_min", "ms/frame", "floor%", "vs off"))
    per = {}
    for nm in ("par", "off", "on"):
        if nm in w: per[nm] = (min(w[nm]["hi"]) - min(w[nm]["lo"])) / (nhi - nlo)
    for nm in ("par", "off", "on"):
        if nm not in per: continue
        lo_s, hi_s = sorted(w[nm]["lo"]), sorted(w[nm]["hi"])
        p2 = (hi_s[1]-lo_s[1])/(nhi-nlo) if len(hi_s) > 1 else per[nm]
        floor = 100.0*(p2-per[nm])/per[nm]
        d = "" if "off" not in per else "  %+.2f%%" % (100.0*(per[nm]-per["off"])/per["off"])
        print("%-5s %9.1f %9.1f %11.3f %7.2f%%%s" % (nm, min(w[nm]["lo"]), min(w[nm]["hi"]), per[nm], floor, d))
PY
