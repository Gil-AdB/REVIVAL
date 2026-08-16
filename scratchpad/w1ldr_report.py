#!/usr/bin/env python3
"""min-of-runs report for w1ldr_ladder.sh / w1ldr_ab.sh.

Prints lighting-w1 / lighting-w2 / DeferredLighting-call / renderFrame for each
stage (or arm) at each pose, with the delta against the reference column.
"""
import sys, re, glob, os
OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/w1/ladder'
POSES = (sys.argv[2] if len(sys.argv) > 2 else '5743').split(',')
PREFIX = sys.argv[3] if len(sys.argv) > 3 else 's'      # 's' = ladder stages, '' = arm names
ARMS = (sys.argv[4] if len(sys.argv) > 4 else '0,1,2').split(',')
ROWS = ['lighting-w1', 'lighting-w2', 'DeferredLighting-call', 'renderFrame']
LABEL = {
    '0': 'FULL (stage 0 == parent binary)',
    '1': 'the WHOLE LDR chain removed',
    '2': 'the TAIL only (viz + clamps + store)',
}


def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)'
                     r'\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m:
            m2 = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+\|'
                          r'\s+(\S+)\s+(\S+)\s+(\S+)', line)
            if not m2:
                continue
        try:
            d[m.group(1).strip()] = {'wall': float(m.group(2)),
                                     'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError:
            pass
    return d


def best_of(pat):
    files = sorted(glob.glob(pat))
    b = None
    for f in files:
        d = parse(f)
        if b is None:
            b = {}
        for k, v in d.items():
            if k not in b:
                b[k] = dict(v)
            else:
                for c in ('wall', 'gi', 'gc'):
                    if v[c] < b[k][c]:
                        b[k][c] = v[c]
    return b, len(files)


for pose in POSES:
    print(f'\n=== t={pose}  ({OUT}) ===')
    print(f'{"arm":<6}{"what":<38}' + ''.join(f'{r:>24}' for r in ROWS))
    ref = None
    for a in ARMS:
        b, n = best_of(f'{OUT}/{PREFIX}{a}_t{pose}_r*.txt')
        if not b:
            continue
        if ref is None:
            ref = b
        cells = []
        for r in ROWS:
            gi = b.get(r, {}).get('gi')
            rg = ref.get(r, {}).get('gi')
            if gi is None:
                cells.append(f'{"-":>24}')
            elif rg and a != ARMS[0]:
                cells.append(f'{gi:>10.4f} ({100*(gi-rg)/rg:+6.2f}%)  ')
            else:
                cells.append(f'{gi:>10.4f}            ')
        print(f'{a:<6}{LABEL.get(a, ""):<38}' + ''.join(cells) + f'  n={n}')
