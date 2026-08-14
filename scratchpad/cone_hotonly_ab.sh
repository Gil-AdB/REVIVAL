#!/bin/zsh
set -u
WT=/Users/gil-ad/work/rev-conevec; cd $WT/Runtime || exit 1
ROUNDS=${1:-5}; OUT=/tmp/hot_ab; mkdir -p $OUT; rm -f $OUT/r*_*.txt
run() { SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$3 \
  --bench=scene@scene=city,t=1961,iters=6 --deferred --texture_filter=1 \
  --profiler=1 --deferred_prof=1 --hw_prof 2>&1 | grep -E '^\[DPROF\]' > $OUT/r$1_$2.txt; }
ARMS=(F2 F1 H2 H1)
for r in $(seq 1 $ROUNDS); do
  n=4; ORD=(); for i in $(seq 0 3); do ORD+=(${ARMS[$(( (i + r) % n + 1 ))]}); done
  for a in $ORD; do case $a in
    F2) run $r F2 DEMO_force2;; F1) run $r F1 DEMO_force1;;
    H2) run $r H2 DEMO_hot2;;   H1) run $r H1 DEMO_hot1;; esac; done
  print -r -- "# round $r (load $(uptime|sed 's/.*averages: //'))"
done
python3 - "$OUT" <<'PY'
import sys,glob,re,os,collections
out=sys.argv[1]; rows=collections.defaultdict(lambda: collections.defaultdict(list))
for f in sorted(glob.glob(os.path.join(out,"r*_*.txt"))):
    arm=os.path.basename(f).split("_",1)[1].split(".")[0]
    for line in open(f):
        m=re.match(r"\[DPROF\]\s+(\S.*?)\s{2,}([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s*\|\s+(\S+)\s+(\S+)\s+(\S+)",line)
        if not m: continue
        if m.group(1).strip() not in ("cones","renderFrame"): continue
        rows[m.group(1).strip()][arm].append((float(m.group(3)),float(m.group(7)),float(m.group(8)),float(m.group(9))))
print(f"{'phase':<12}{'arm':<4}{'wall_min':>9}{'Ginstr/f':>10}{'Gcyc/f':>9}{'IPC':>7}{'n':>4}")
for name in ("cones","renderFrame"):
    agg={}
    for arm in ("F2","F1","H2","H1"):
        v=rows[name].get(arm)
        if not v: continue
        agg[arm]=(min(x[0] for x in v),min(x[1] for x in v),min(x[2] for x in v),sum(x[3] for x in v)/len(v),len(v))
        w,gi,gc,ipc,n=agg[arm]; print(f"{name:<12}{arm:<4}{w:>9.3f}{gi:>10.3f}{gc:>9.3f}{ipc:>7.3f}{n:>4}")
    for b,nw in (("F2","H2"),("F1","H1"),("F2","F1"),("H2","H1")):
        if b in agg and nw in agg:
            x,y=agg[b],agg[nw]
            print(f"   {b}->{nw:<6}{y[0]-x[0]:>+9.3f}{y[1]-x[1]:>+10.3f}{y[2]-x[2]:>+9.3f}  ({100*(y[1]-x[1])/x[1]:+.1f}% instr, {100*(y[2]-x[2])/x[2]:+.1f}% cyc)")
    print()
PY
