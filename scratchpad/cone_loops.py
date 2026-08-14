#!/usr/bin/env python3
"""Loop-nest census of a disassembled cone kernel.

Backward branches give the loop back-edges; an instruction's depth is the number
of [target, branch] intervals containing it.  Round 5 needs this because the
static whole-function histogram mixes the per-tile prologue with the per-(batch
x spot) inner loop, and only the latter is the 3.2 M/frame hot path.

usage: cone_loops.py <disasm.asm> [--depth N] [--range LO:HI]
"""
import re, sys, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cone_census import classify, VEC_ALU

def load(path):
    ins = []
    for l in open(path):
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', l)
        if m: ins.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return ins

def main():
    path = sys.argv[1]
    args = sys.argv[2:]
    want_depth = None; rng = None
    i = 0
    while i < len(args):
        if args[i] == '--depth': want_depth = int(args[i+1]); i += 2
        elif args[i] == '--range':
            a, b = args[i+1].split(':'); rng = (int(a,16), int(b,16)); i += 2
        else: i += 1
    ins = load(path)
    idx = {a: k for k, (a, o, g) in enumerate(ins)}
    # back-edges
    edges = []
    for k, (a, op, g) in enumerate(ins):
        if op.startswith('b') and not op.startswith('bl') and not op.startswith('bsl') \
           and not op.startswith('bic') and not op.startswith('bif') and not op.startswith('bit'):
            m = re.search(r'0x([0-9a-f]+)', g)
            if m:
                t = int(m.group(1), 16)
                if t <= a and t in idx:
                    edges.append((idx[t], k))
    depth = [0]*len(ins)
    for t, k in edges:
        for j in range(t, k+1): depth[j] += 1
    # report
    bymax = collections.Counter()
    for k in range(len(ins)): bymax[depth[k]] += 1
    print(f"loop back-edges: {len(edges)};  instructions by nesting depth:")
    for d in sorted(bymax): print(f"   depth {d}: {bymax[d]:5d} instructions")
    print("\nback-edge intervals (start,end,#instr,depth-at-start):")
    for t, k in sorted(edges, key=lambda e: e[1]-e[0], reverse=True)[:14]:
        print(f"   0x{ins[t][0]:x} .. 0x{ins[k][0]:x}   {k-t+1:5d} instr  depth {depth[t]}")
    sel = range(len(ins))
    if want_depth is not None:
        sel = [k for k in range(len(ins)) if depth[k] >= want_depth]
    if rng:
        sel = [k for k in sel if rng[0] <= ins[k][0] <= rng[1]]
    b = collections.Counter(); byop = collections.defaultdict(collections.Counter)
    for k in sel:
        a, op, g = ins[k]; c = classify(op, g); b[c] += 1; byop[c][op] += 1
    tot = len(sel); valu = sum(b[x] for x in VEC_ALU)
    lab = (f"depth>={want_depth}" if want_depth is not None else "all") + \
          (f" range 0x{rng[0]:x}-0x{rng[1]:x}" if rng else "")
    print(f"\n=== {lab}: {tot} instructions, VECTOR-ALU = {valu} ===")
    for k in list(VEC_ALU) + ['vec-divsqrt','mem-vec','mem-scalar','scalar-fp','scalar-int','branch']:
        if b[k]:
            det = ' '.join(f"{o}:{c}" for o, c in byop[k].most_common(9))
            print(f" {'*' if k in VEC_ALU else ' '}{k:<15}{b[k]:>5}  {det}")

if __name__ == '__main__':
    main()
