#!/usr/bin/env python3
"""Full three-arm table for w1ldr_ab.sh: Gi/f, Gcyc/f and wall_min, min-of-runs."""
import sys, re, glob
OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/w1/ab1'
POSES = (sys.argv[2] if len(sys.argv) > 2 else '5743,2845,6097,3409,5813').split(',')
ARMS = ['par', 'off', 'on']
ROWS = ['lighting-w1', 'lighting-w2', 'DeferredLighting-call', 'renderFrame']
RX = re.compile(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+(\S+)'
                r'\s+\|\s+(\S+)\s+(\S+)\s+(\S+)')


def best_of(pat):
    b, n = {}, 0
    for f in sorted(glob.glob(pat)):
        n += 1
        for line in open(f):
            m = RX.match(line)
            if not m:
                continue
            k = m.group(1).strip()
            try:
                v = {'wall': float(m.group(3)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
            except ValueError:
                continue
            if k not in b:
                b[k] = v
            else:
                for c in v:
                    if v[c] < b[k][c]:
                        b[k][c] = v[c]
    return b, n


print('| pose | row | par | off | **ON** |')
print('|---|---|--:|--:|--:|')
for pose in POSES:
    d = {}
    for a in ARMS:
        d[a], n = best_of(f'{OUT}/{a}_t{pose}_r*.txt')
    for r in ROWS:
        for col, lab in (('gi', 'Gi/f'), ('gc', 'Gcyc/f'), ('wall', 'wall')):
            if col != 'gi' and r not in ('lighting-w1', 'renderFrame'):
                continue
            p = d['par'].get(r, {}).get(col)
            o = d['off'].get(r, {}).get(col)
            n2 = d['on'].get(r, {}).get(col)
            if p is None:
                continue
            f = '%.3f' if col != 'wall' else '%.2f'
            print(f'| t={pose} | `{r}` {lab} | {f % p} | '
                  f'{f % o} ({100*(o-p)/p:+.2f} %) | **{f % n2} ({100*(n2-p)/p:+.2f} %)** |')
