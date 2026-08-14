#!/bin/zsh
# Round 5: the STATIC op census, read off the ablation ladder.
#
# census(ABLATE=n) - census(ABLATE=n-1) is the op mix of stage n, because the
# `continue` at stage n dead-code-eliminates everything after it.  Compiles the
# TU to assembly only (no link) with the single-arm control defines, so a stage
# costs ~10 s instead of a full rebuild.
#
#   usage: cone_census_ladder.sh [stages...]     (default: the whole ladder)
set -u
WT=/Users/gil-ad/work/rev-conevec
S=/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad
if (( $# )); then STAGES=($@); else STAGES=(1 2 3 4 6 7 8 9 10 5 0); fi
for st in $STAGES; do
  /usr/bin/c++ -DFDS_DEV=0 -DSIMDE_ENABLE_NATIVE_ALIASES -D_CRT_SECURE_NO_WARNINGS \
    -D_LIB -D_MBCS -I$WT/FDS -DFDS_CONE_FORCE=1 -DFDS_CONE_HOTONLY=1 \
    -DFDS_CONE_ABLATE=$st ${EXTRA:-} \
    -O3 -DNDEBUG -std=c++20 -arch arm64 -ffp-contract=fast -g -S \
    -o $S/abl_$st.s $WT/FDS/RENDER/DeferredVolumetric.cpp 2>/dev/null || \
      { print -r -- "!! compile failed stage $st"; exit 2; }
done
python3 - "$S" "${STAGES[*]}" <<'PY'
import sys, os, collections
sys.path.insert(0, '/Users/gil-ad/work/rev-conevec/scratchpad')
from cone_census import classify, VEC_ALU
from cone_srcmap import parse
S, stages = sys.argv[1], [int(x) for x in sys.argv[2].split()]
LAB = {1:"per-batch floor", 2:"+per-spot scalar prologue", 3:"+8-wide SOLVE",
       4:"+per-lane dz/fade", 6:"+body broadcasts/quad/rsqrt/args",
       7:"+body atanDiff", 8:"+body midpoint cone/fade/softEdge",
       9:"+body vAcc chain", 10:"+body noise loop", 5:"+body masks/shadow tap",
       0:"FULL (+colour accumulate)"}
cens = {}
for st in stages:
    ins = parse(os.path.join(S, f"abl_{st}.s"))
    c = collections.Counter()
    for _, op, a, ln, dv in ins: c[classify(op, a)] += 1
    c['TOTAL'] = len(ins)
    c['VALU'] = sum(c[k] for k in VEC_ALU)
    cens[st] = c
cols = ['VALU','vec-alu-arith','vec-mov','vec-cmp','vec-logic','vec-blend',
        'vec-const','vec-dup-ins','vec-permute','vec-alu-int','vec-divsqrt',
        'mem-vec','scalar-fp','scalar-int','TOTAL']
hdr = f"{'stage':<34}" + ''.join(f"{c.replace('vec-',''):>9}" for c in cols)
print(hdr); print('-'*len(hdr))
prev = None
for st in stages:
    c = cens[st]
    row = f"{str(st)+' '+LAB.get(st,''):<34}" + ''.join(f"{c[k]:>9}" for k in cols)
    print(row)
print()
print("INCREMENTS (stage cost in emitted ops)")
print(hdr); print('-'*len(hdr))
order = [(1,None),(2,1),(3,2),(4,3),(6,4),(7,6),(8,7),(9,8),(10,9),(5,10),(0,5)]
for st, base in order:
    if st not in cens: continue
    c = cens[st]; b = cens[base] if base in cens else collections.Counter()
    row = f"{str(st)+' '+LAB.get(st,''):<34}" + ''.join(f"{c[k]-b[k]:>+9}" for k in cols)
    print(row)
PY
