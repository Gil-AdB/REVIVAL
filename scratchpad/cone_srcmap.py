#!/usr/bin/env python3
"""Bucket the cone kernel's emitted ops by functional class AND by source line.

Input: a `clang -S -g` assembly listing of DeferredVolumetric.cpp (the .loc
directives carry the line map that a thin-LTO'd linked binary loses).

usage: cone_srcmap.py <file.s> [--region L0:L1] [--top N] [--bucket NAME]
"""
import re, sys, collections, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cone_census import classify, VEC_ALU

FUNC = '_ZL27Render_VolumetricCones_Tile'

def parse(path):
    """-> list of (idx, op, args, line, label_at_or_before)"""
    out = []
    inside = False
    cur_line = 0
    files = {}
    label = None
    with open(path, errors='replace') as fh:
        for raw in fh:
            s = raw.strip()
            if s.startswith('.file'):
                m = re.match(r'\.file\s+(\d+)\s+"([^"]*)"\s*"([^"]*)"', s)
                if m: files[int(m.group(1))] = m.group(3)
                globals()['FILES'] = files
                continue
            if not inside:
                if s.startswith('_' + FUNC) and ':' in s:
                    inside = True
                continue
            if s.startswith('.loc'):
                m = re.match(r'\.loc\s+(\d+)\s+(\d+)', s)
                if m:
                    cur_line = (int(m.group(1)), int(m.group(2)))
                    if cur_line[0] == 0 and cur_line[1]:
                        globals()['LAST_DV'] = cur_line[1]
                continue
            if s.startswith('.cfi_endproc'):
                break
            if s.startswith('.') or s.startswith(';'):
                continue
            if re.match(r'^[A-Za-z_.$][\w.$]*:', s):
                label = s.split(':')[0]
                continue
            if not s:
                continue
            s = s.split(';')[0].rstrip()
            if not s: continue
            parts = s.split(None, 1)
            op = parts[0]
            args = parts[1] if len(parts) > 1 else ''
            out.append((len(out), op, args, cur_line, globals().get('LAST_DV', 0)))
    return out

FILES = {}

def fmt(key, src):
    if not key: return "<no .loc>"
    fi, ln = key
    if fi == 0:
        txt = src[ln-1].strip()[:64] if 0 < ln <= len(src) else '?'
        return f"DV.cpp:{ln} {txt}"
    return f"{FILES.get(fi, '?'+str(fi))}:{ln}"

def main():
    path = sys.argv[1]
    args = sys.argv[2:]
    region = None
    top = 40
    onlyb = None
    i = 0
    while i < len(args):
        if args[i] == '--region':
            a, b = args[i+1].split(':'); region = (int(a), int(b)); i += 2
        elif args[i] == '--top': top = int(args[i+1]); i += 2
        elif args[i] == '--bucket': onlyb = args[i+1]; i += 2
        else: i += 1

    ins = parse(path)
    if region:
        ins = [x for x in ins if region[0] <= x[4] <= region[1]]
    buckets = collections.Counter()
    byline = collections.defaultdict(collections.Counter)
    lines_valu = collections.Counter()
    for _, op, a, ln, lab in ins:
        k = classify(op, a)
        buckets[k] += 1
        byline[ln][op] += 1
        if k in VEC_ALU: lines_valu[ln] += 1
    tot = len(ins)
    valu = sum(buckets[k] for k in VEC_ALU)
    print(f"=== {os.path.basename(path)}"
          + (f" lines {region[0]}-{region[1]}" if region else "")
          + f": {tot} instructions, VECTOR-ALU = {valu} ===")
    for k in list(VEC_ALU) + ['vec-divsqrt','mem-vec','mem-scalar','scalar-fp','scalar-int','branch']:
        if buckets[k]: print(f"  {'*' if k in VEC_ALU else ' '}{k:<16}{buckets[k]:>6}")

    src = open('/Users/gil-ad/work/rev-conevec/FDS/RENDER/DeferredVolumetric.cpp', errors='replace').read().split('\n')
    if onlyb:
        print(f"\n--- sites emitting bucket '{onlyb}', by source line ---")
        cnt = collections.Counter()
        det = collections.defaultdict(collections.Counter)
        for _, op, a, ln, lab in ins:
            if classify(op, a) == onlyb:
                cnt[ln] += 1; det[ln][op] += 1
        for ln, c in cnt.most_common(top):
            print(f"  {fmt(ln, src):<44}{c:>4}  {dict(det[ln])}")
    else:
        print(f"\n--- top {top} source lines by VECTOR-ALU ops ---")
        for ln, c in lines_valu.most_common(top):
            ops = ' '.join(f"{o}:{n}" for o, n in byline[ln].most_common(6))
            print(f"  {fmt(ln, src):<44}{c:>4} valu | {ops}")

if __name__ == '__main__':
    main()
