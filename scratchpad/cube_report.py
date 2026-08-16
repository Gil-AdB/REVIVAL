#!/usr/bin/env python3
"""Report the CUBE-TAP INTERIOR ablation ladder (min over runs, per stage)."""
import sys, re, glob

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/cube/ladderA'
POSES = sys.argv[2].split(',') if len(sys.argv) > 2 else ['5743', '2845']
LABEL = {'x': 'NO TAP AT ALL (omni cut 8)', 1: 'call frame + cubeIdx guard',
         2: '+ lightISource, 3 world subs', 3: '+ SelectFace',
         4: '+ face map resolve', 5: '+ viewToLight 3x3 matmul',
         6: '+ lz near reject', 7: '+ 2 face-frustum rejects',
         8: '+ 1/lz, smX/smY project', 9: '+ int trunc + bounds reject',
         10: '+ uniformity pyramid', 11: '+ bilinear weights',
         12: '+ 2x2 tap addressing', 0: 'FULL (+ 4 taps, accumulate)'}
ORDER = ['x', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0]

def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m: continue
        try: d[m.group(1).strip()] = {'wall': float(m.group(3)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError: pass
    return d

TAPS = {'5743': 2.943e6, '2845': None}
for pose in POSES:
    print(f'\n=== t={pose}  ({OUT}) ===')
    vals = {}
    for st in ORDER:
        best = None
        for f in sorted(glob.glob(f'{OUT}/s{st}_t{pose}_r*.txt')):
            d = parse(f)
            if 'lighting-w1' not in d: continue
            if best is None or d['lighting-w1']['gi'] < best['lighting-w1']['gi']: best = d
        if best: vals[st] = best
    if 'x' in vals and 0 in vals:
        tap = vals[0]['lighting-w1']['gi'] - vals['x']['lighting-w1']['gi']
    else:
        tap = None
    print(f'{"st":>3} {"what is KEPT":<32} {"w1 Gi/f":>9} {"dGi":>8} {"%tap":>7} {"i/tap":>7} {"w1 Gcyc":>8}')
    prev = None
    for st in ORDER:
        v = vals.get(st)
        if not v: continue
        gi = v['lighting-w1']['gi']
        d = (gi - prev) if prev is not None else 0.0
        pct = (100.0 * d / tap) if (tap and prev is not None) else 0.0
        n = TAPS.get(pose)
        ipt = (d * 1e9 / n) if (n and prev is not None) else 0.0
        print(f'{str(st):>3} {LABEL[st]:<32} {gi:9.3f} {d:8.3f} {pct:6.1f}% {ipt:7.1f} {v["lighting-w1"]["gc"]:8.3f}')
        prev = gi
    if tap: print(f'    WHOLE TAP = {tap:.3f} Gi/f'
                  + (f'  = {tap*1e9/TAPS[pose]:.0f} instr/tap over {TAPS[pose]/1e6:.3f} M taps' if TAPS.get(pose) else ''))
