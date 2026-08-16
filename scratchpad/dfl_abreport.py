#!/usr/bin/env python3
"""min-of-rounds report for dfl_ab.sh output (round 1 dropped)."""
import sys, re, glob, os
OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/dfl/ab1'
ARMS = sys.argv[2].split(',') if len(sys.argv) > 2 else ['par','off','on','cube','lmad','fill']
POSES = sys.argv[3].split(',') if len(sys.argv) > 3 else ['5743','2845','6097']
ROWS = ['renderFrame','DeferredLighting-call','lighting-w1','lighting-w2','frame']
def parse(f):
    d = {}
    for line in open(f):
        m = re.match(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if not m: continue
        try: d[m.group(1).strip()] = {'wall': float(m.group(3)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError: pass
    return d
for pose in POSES:
    print(f'\n=== greets t={pose} — min over rounds (r1 dropped) ===')
    hdr = f'{"row":<24}' + ''.join(f'{a:>12}' for a in ARMS)
    print(hdr)
    data = {}
    for a in ARMS:
        best = {}
        for f in sorted(glob.glob(f'{OUT}/{a}_t{pose}_r*.txt')):
            if re.search(r'_r1\.txt$', f): continue
            d = parse(f)
            for k, v in d.items():
                if k not in best: best[k] = dict(v)
                else:
                    for c in ('wall','gi','gc'):
                        if v[c] < best[k][c]: best[k][c] = v[c]
        data[a] = best
    for row in ['renderFrame','DeferredLighting-call','lighting-w1','lighting-w2']:
        for col, lbl in (('gi','Gi/f'), ('gc','Gcyc/f'), ('wall','wall')):
            line = f'{row[:20]+" "+lbl:<24}'
            base = data[ARMS[0]].get(row, {}).get(col)
            for a in ARMS:
                v = data[a].get(row, {}).get(col)
                if v is None: line += f'{"-":>12}'
                elif a == ARMS[0]: line += f'{v:>12.3f}'
                else: line += f'{v:>8.3f}{100.0*(v-base)/base:+5.1f}%'.rjust(12)
            print(line)
        print()
