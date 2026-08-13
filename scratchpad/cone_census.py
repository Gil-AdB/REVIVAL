#!/usr/bin/env python3
"""Bucket every instruction of a disassembled cone kernel by functional class.

usage: cone_census.py <disasm.asm> [label]      (asm from objdump --no-show-raw-insn)

Round 5's instrument: the metric on this pass is VECTOR-ALU OP COUNT (see
docs/HW_PROFILING.md section 12).  This classifies each emitted op into the
bucket that decides which pipe it issues on, so a spelling that costs two
vector-ALU ops where the operation needs one shows up as a bucket delta.
"""
import re, sys, collections

VEC_SUFFIX = re.compile(r'\.(2s|4s|2d|8b|16b|4h|8h|2h|8s)$')

def classify(op, args):
    b = op.split('.')[0]
    # ---- memory -------------------------------------------------------
    if b in ('ldr','ldur','ldp','ldrb','ldrh','ldrsw','ldrsb','ldrsh',
             'str','stur','stp','strb','strh','ld1','st1','ld2','st2',
             'ldnp','stnp','ldar','stlr','prfm','ldursw'):
        vec = args.lstrip().startswith(('q','v','d','s','{'))
        return ('mem-vec' if vec else 'mem-scalar')
    # ---- control ------------------------------------------------------
    if b in ('b','bl','br','blr','ret','cbz','cbnz','tbz','tbnz') or \
       (b.startswith('b.') ) or b in ('brk','nop','hint','svc'):
        return 'branch'
    # ---- vector ops ---------------------------------------------------
    isvec = bool(VEC_SUFFIX.search(op)) or args.lstrip().startswith('v')
    if isvec:
        if b in ('mov','orr') and ('.16b' in op or '.8b' in op):
            # mov.16b vD, vN is an alias of orr; a pure register copy
            return 'vec-mov'
        if b in ('fmla','fmls','fmul','fadd','fsub','fneg','fabs','fmadd',
                 'fnmul','fmulx','frecpe','frecps','frsqrte','frsqrts',
                 'fmax','fmin','fmaxnm','fminnm','frinta','frintm','frintp',
                 'frintz','frintn','frintx'):
            return 'vec-alu-arith'
        if b in ('add','sub','mul','neg','abs','shl','sshr','ushr','sshl',
                 'ushl','ssra','usra','sqadd','uqadd','mla','mls','sabd'):
            return 'vec-alu-int'
        if b in ('and','bic','orn','eor','not','mvn'):
            return 'vec-logic'
        if b in ('bsl','bit','bif','sel'):
            return 'vec-blend'
        if b in ('fcmeq','fcmgt','fcmge','fcmlt','fcmle','cmeq','cmgt','cmge',
                 'cmhi','cmhs','cmtst','fcmp','fcmpe'):
            return 'vec-cmp'
        if b in ('dup','ins','mov') and not ('.16b' in op or '.8b' in op):
            # dup.4s v, w / dup.4s v, v[i] / ins  -- broadcast or lane insert
            return 'vec-dup-ins'
        if b in ('zip1','zip2','uzp1','uzp2','trn1','trn2','ext','tbl','tbx',
                 'rev32','rev64','xtn','xtn2','sqxtn','uqxtn'):
            return 'vec-permute'
        if b in ('fcvt','fcvtzs','fcvtzu','fcvtn','fcvtl','fcvtms','fcvtns',
                 'fcvtps','fcvtas','scvtf','ucvtf','fcvtxn'):
            return 'vec-convert'
        if b in ('fdiv','fsqrt','frsqrt'):
            return 'vec-divsqrt'
        if b in ('addv','faddp','fmaxv','fminv','fmaxnmv','fminnmv','umaxv',
                 'uminv','saddlv','uaddlv','addp'):
            return 'vec-reduce'
        if b in ('movi','mvni','fmov'):
            return 'vec-const'
        if b in ('umov','smov'):
            return 'vec-extract'
        return 'vec-other'
    # ---- scalar -------------------------------------------------------
    if b in ('fmul','fadd','fsub','fdiv','fsqrt','fneg','fabs','fmadd','fmsub',
             'fnmadd','fmax','fmin','fcvt','fcvtzs','scvtf','fcsel','fccmp',
             'fcmp','fmov','frinta','frintm','frintz'):
        return 'scalar-fp'
    return 'scalar-int'

# vector-ALU buckets: what issues on the 4 NEON/FP pipes and is the metric
VEC_ALU = ('vec-alu-arith','vec-alu-int','vec-logic','vec-blend','vec-cmp',
           'vec-dup-ins','vec-permute','vec-convert','vec-reduce','vec-mov',
           'vec-const','vec-extract','vec-other')

def main():
    f = sys.argv[1]; lab = sys.argv[2] if len(sys.argv) > 2 else f
    buckets = collections.Counter(); ops = collections.Counter()
    byop = collections.defaultdict(collections.Counter)
    for L in open(f):
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', L)
        if not m: continue
        op, args = m.group(2), m.group(3)
        k = classify(op, args)
        buckets[k] += 1; ops[op] += 1; byop[k][op] += 1
    tot = sum(buckets.values())
    valu = sum(buckets[k] for k in VEC_ALU)
    print(f"=== {lab}: {tot} instructions, VECTOR-ALU = {valu} ({100*valu/tot:.1f}%) ===")
    order = list(VEC_ALU) + ['vec-divsqrt','mem-vec','mem-scalar','scalar-fp',
                             'scalar-int','branch']
    for k in order:
        if not buckets[k]: continue
        det = ' '.join(f"{o}:{c}" for o, c in byop[k].most_common(8))
        star = '*' if k in VEC_ALU else ' '
        print(f"{star}{k:<16}{buckets[k]:>6}   {det}")

if __name__ == '__main__':
    main()
