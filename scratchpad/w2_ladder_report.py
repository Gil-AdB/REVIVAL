#!/usr/bin/env python3
"""min-of-runs report for w2_ladder.sh: stage totals + per-stage deltas."""
import sys, re, glob
OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/w2/ladder'
POSE = sys.argv[2] if len(sys.argv) > 2 else '5743'
LABEL = {
    1: 'parity test only (w2 loop skeleton)',
    2: '+ z alive test',
    3: '+ mirrorId, mat32, sentinel, matIDc',
    4: '+ envForceFull / env_full drop',
    5: '+ centre normal decode',
    6: '+ neighbour index setup',
    7: '+ centre fetchTexel (haveOwn)',
    8: '+ neighbour compat + accumulate loop',
    9: '+ average write-out',
    0: 'FULL (adds the full-shade fallback)',
}
def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m: continue
        try: d[m.group(1).strip()] = {'wall': float(m.group(3)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError: pass
    return d
best = {}
for st in list(range(1, 10)) + [0]:
    files = sorted(glob.glob(f'{OUT}/s{st}_t{POSE}_r*.txt'))
    if not files: continue
    b = None
    for f in files:
        d = parse(f)
        for k, v in d.items():
            if b is None: b = {}
            if k not in b: b[k] = dict(v)
            else:
                for c in ('wall', 'gi', 'gc'):
                    if v[c] < b[k][c]: b[k][c] = v[c]
    best[st] = b
order = [s for s in list(range(1, 10)) + [0] if s in best]
print(f'{"stage":<6}{"what":<40}{"w2 Gi/f":>10}{"delta":>9}{"w2 Gcyc":>10}{"call Gi/f":>11}')
prev = None
for st in order:
    gi = best[st].get('lighting-w2', {}).get('gi')
    gc = best[st].get('lighting-w2', {}).get('gc')
    cg = best[st].get('DeferredLighting-call', {}).get('gi')
    d = '' if prev is None or gi is None else f'{gi-prev:+.4f}'
    print(f'{st:<6}{LABEL.get(st,""):<40}{gi if gi is not None else 0:>10.4f}{d:>9}{gc if gc is not None else 0:>10.4f}{cg if cg is not None else 0:>11.4f}')
    prev = gi
