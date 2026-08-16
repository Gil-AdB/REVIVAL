#!/usr/bin/env python3
"""Report the omni ablation ladder from /tmp/dfl/ladderX (min over runs, per stage)."""
import sys, os, re, glob

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/dfl/ladderA'
POSES = sys.argv[2].split(',') if len(sys.argv) > 2 else ['5743', '2845']
ROWS = ['renderFrame', 'DeferredLighting-call', 'lighting-w1', 'lighting-w2']
LABEL = {1:'loop floor (loop deleted)', 2:'+ mirrorId test', 3:'+ w, N.L dot, dot<0',
         4:'+ len2, range', 5:'+ bounce portal', 6:'+ rsqrt/dist/k', 7:'+ spot cone',
         8:'+ computeMapShadowAtten', 9:'+ cube tap', 10:'+ relief horizon',
         11:'+ diffuse accumulate', 0:'FULL (+ specular lobe)'}

def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m: continue
        name = m.group(1).strip()
        try: d[name] = {'wall': float(m.group(2)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError: pass
    return d

for pose in POSES:
    print(f'\n=== t={pose}  ({OUT}) ===')
    print(f'{"st":>3} {"what is KEPT":<28} {"w1 Gi/f":>9} {"dGi":>8} {"%loop":>7} {"w1 Gcyc":>8} {"w2 Gi":>7} {"call Gi":>8} {"rF Gi":>7}')
    vals = {}
    for st in list(range(0, 12)):
        fs = sorted(glob.glob(f'{OUT}/s{st}_t{pose}_r*.txt'))
        best = None
        for f in fs:
            d = parse(f)
            if 'lighting-w1' not in d: continue
            if best is None or d['lighting-w1']['gi'] < best['lighting-w1']['gi']: best = d
        if best: vals[st] = best
    full = vals.get(0)
    floor = vals.get(1)
    loop = (full['lighting-w1']['gi'] - floor['lighting-w1']['gi']) if (full and floor) else None
    order = [1,2,3,4,5,6,7,8,9,10,11,0]
    prev = None
    for st in order:
        v = vals.get(st)
        if not v: continue
        gi = v['lighting-w1']['gi']
        d = (gi - prev) if prev is not None else 0.0
        pct = (100.0*d/loop) if (loop and prev is not None) else 0.0
        print(f'{st:>3} {LABEL[st]:<28} {gi:9.3f} {d:8.3f} {pct:6.1f}% {v["lighting-w1"]["gc"]:8.3f} '
              f'{v["lighting-w2"]["gi"]:7.3f} {v["DeferredLighting-call"]["gi"]:8.3f} {v["renderFrame"]["gi"]:7.3f}')
        prev = gi
    if loop: print(f'    omni loop total = {loop:.3f} Gi/f  ({100.0*loop/full["lighting-w1"]["gi"]:.1f}% of w1)')
