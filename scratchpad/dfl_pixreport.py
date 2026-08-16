#!/usr/bin/env python3
import sys, re, glob
OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/dfl/pixA'
POSES = sys.argv[2].split(',') if len(sys.argv) > 2 else ['5743', '2845']
LABEL = {1:'z alive test', 2:'+ mirrorId/mat32/shadowId', 3:'+ albedo fetch + tint',
         4:'+ lightmap ADDRESS resolve', 5:'+ normal decode / TBN',
         6:'+ view pos + SH ambient', 7:'+ view dir + PBR consts', 8:'+ AO fetch',
         9:'+ sample world position', 10:'+ POM horizon record',
         0:'FULL (+ omni loop + compose)'}
def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m: continue
        try: d[m.group(1).strip()] = {'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError: pass
    return d
for pose in POSES:
    print(f'\n=== t={pose}  ({OUT}) ===')
    print(f'{"st":>3} {"what is KEPT":<30} {"w1 Gi/f":>9} {"dGi":>8} {"%floor":>8} {"call Gi":>8} {"rF Gi":>7}')
    vals = {}
    for st in range(0, 11):
        best = None
        for f in sorted(glob.glob(f'{OUT}/s{st}_t{pose}_r*.txt')):
            d = parse(f)
            if 'lighting-w1' not in d: continue
            if best is None or d['lighting-w1']['gi'] < best['lighting-w1']['gi']: best = d
        if best: vals[st] = best
    FLOOR = 0.473 if pose == '5743' else 0.474   # omni-ladder stage 1
    prev = None
    for st in [1,2,3,4,5,6,7,8,9,10,0]:
        v = vals.get(st)
        if not v: continue
        gi = v['lighting-w1']['gi']
        d = (gi - prev) if prev is not None else 0.0
        print(f'{st:>3} {LABEL[st]:<30} {gi:9.3f} {d:8.3f} {100.0*d/FLOOR:7.1f}% '
              f'{v["DeferredLighting-call"]["gi"]:8.3f} {v["renderFrame"]["gi"]:7.3f}')
        prev = gi
    if 10 in vals and 0 in vals:
        preloop = vals[10]['lighting-w1']['gi']
        full = vals[0]['lighting-w1']['gi']
        loop = 1.247 if pose == '5743' else 1.108
        print(f'    pre-loop body = {preloop:.3f}; omni loop = {loop:.3f}; '
              f'COMPOSE = full - preloop - loop = {full - preloop - loop:.3f} Gi/f')
