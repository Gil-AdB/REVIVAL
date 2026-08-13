#!/bin/zsh
set -u
cd /Users/gil-ad/work/rev-conevec/Runtime || exit 1
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ROUNDS=${1:-3}; OUT=/tmp/cone_sweep3; mkdir -p $OUT; rm -f $OUT/*.txt
run() { ./$3 --bench=scene@scene=city,t=$4,iters=6 --deferred --texture_filter=1 \
  --profiler=1 --deferred_prof=1 --hw_prof 2>&1 | grep -E '^\[DPROF\]' > $OUT/t$4_r$1_$2.txt; }
for t in 400 900 1400 1961 2400; do
  for r in $(seq 1 $ROUNDS); do
    ARMS=(F2 F1 H2); n=3; ORD=()
    for i in $(seq 0 2); do ORD+=(${ARMS[$(( (i + r) % n + 1 ))]}); done
    for a in $ORD; do case $a in
      F2) run $r F2 DEMO_force2 $t;; F1) run $r F1 DEMO_force1 $t;; H2) run $r H2 DEMO_hot2 $t;; esac; done
  done
  print -r -- "# t=$t done (load $(uptime|sed 's/.*averages: //'))"
done
python3 - "$OUT" <<'PY'
import sys,glob,re,os,collections
out=sys.argv[1]; d=collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out,"t*_r*_*.txt"))):
    b=os.path.basename(f)[:-4].split("_"); t=int(b[0][1:]); arm=b[2]
    for line in open(f):
        m=re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)",line)
        if not m or m.group(1).strip()!="cones": continue
        d[t][arm].append((float(m.group(3)),float(m.group(7)),float(m.group(8)),float(m.group(9))))
print(f"{'t':>6} | {'F2 unfused':^26} | {'F1 fused':^26} | {'H2 hot-only':^26}")
print(f"{'':>6} | {'wall':>7}{'Ginstr':>9}{'Gcyc':>8} | {'wall':>7}{'Ginstr':>9}{'Gcyc':>8} | {'wall':>7}{'Ginstr':>9}{'Gcyc':>8}")
print("-"*94)
for t in sorted(d):
    row=f"{t:>6} |"
    for arm in ("F2","F1","H2"):
        v=d[t].get(arm)
        if not v: row+=f"{'':>26} |"; continue
        row+=f"{min(x[0] for x in v):>7.2f}{min(x[1] for x in v):>9.3f}{min(x[2] for x in v):>8.3f} |"
    print(row)
print("\ndeltas vs F2 (fusion, and cold-branch removal):")
print(f"{'t':>6} {'F2->F1 instr':>13}{'F2->F1 cyc':>12}{'F2->H2 instr':>14}{'F2->H2 cyc':>12}{'F2->H2 wall':>13}")
for t in sorted(d):
    g=lambda a,i: min(x[i] for x in d[t][a]) if d[t].get(a) else float('nan')
    print(f"{t:>6} {100*(g('F1',1)-g('F2',1))/g('F2',1):>12.1f}%{100*(g('F1',2)-g('F2',2))/g('F2',2):>11.1f}%"
          f"{100*(g('H2',1)-g('F2',1))/g('F2',1):>13.1f}%{100*(g('H2',2)-g('F2',2))/g('F2',2):>11.1f}%"
          f"{100*(g('H2',0)-g('F2',0))/g('F2',0):>12.1f}%")
PY
