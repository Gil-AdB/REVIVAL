#!/usr/bin/env python3
"""Report the three-arm --deferred_cube_prepass A/B (min over rounds)."""
import sys, re, glob

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/cube/ab'
POSES = sys.argv[2].split(',') if len(sys.argv) > 2 else ['5743','2845','6097','3409','5813']
ROWS = [('lighting-w1','gi'), ('lighting-w1','gc'), ('lighting-w1','wall'), ('lighting-w1','thr'),
        ('lighting-w2','gi'),
        ('DeferredLighting-call','gi'), ('DeferredLighting-call','wall'),
        ('renderFrame','gi'), ('renderFrame','wall')]

def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m: continue
        try:
            e = {'wall': float(m.group(3)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError:
            continue
        try: e['thr'] = float(m.group(5))   # '-' on depth-0/1 rows
        except ValueError: pass
        d[m.group(1).strip()] = e
    return d

for pose in POSES:
    best = {}
    for arm in ('par','off','on'):
        for f in sorted(glob.glob(f'{OUT}/{arm}_t{pose}_r*.txt')):
            d = parse(f)
            if 'lighting-w1' not in d: continue
            for row, col in ROWS:
                if row not in d: continue
                k = (arm, row, col)
                v = d[row][col]
                if k not in best or v < best[k]: best[k] = v
    if not best: continue
    print(f'\n=== greets t={pose}, 1512x848, his acceptance arm ===')
    print(f'{"row":<32} {"par":>9} {"off":>9} {"%":>7} {"ON":>9} {"%par":>7} {"%off":>7}')
    for row, col in ROWS:
        p = best.get(('par',row,col)); o = best.get(('off',row,col)); n = best.get(('on',row,col))
        if p is None or o is None or n is None: continue
        print(f'{row+" "+col:<32} {p:9.3f} {o:9.3f} {100*(o-p)/p:+6.2f}% {n:9.3f} '
              f'{100*(n-p)/p:+6.2f}% {100*(n-o)/o:+6.2f}%')
