#!/usr/bin/env python3
"""min-of-rounds report for w2oct_ab.sh, with a per-column NOISE FLOOR.

The floor is the arm's own round-to-round spread on the kept rounds,
(max-min)/min, printed next to the min. A delta smaller than the floor of the
columns it is drawn from is not a result — the spot round (2026-08-16n) is the
precedent for saying so out loud instead of quoting an LSB as a win.
"""
import sys, re, glob
OUT   = sys.argv[1] if len(sys.argv) > 1 else '/tmp/w2oct/ab1'
ARMS  = sys.argv[2].split(',') if len(sys.argv) > 2 else ['par','h4off','h4on','f4','f2']
POSES = sys.argv[3].split(',') if len(sys.argv) > 3 else ['2845','3409','5743','5813','6097']
ROWS  = ['renderFrame','DeferredLighting-call','lighting-w1','lighting-w2']
PAT = re.compile(r'\[DPROF\]\s+(\S[^|]*?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\S+)\s+(\S+)\s+\|\s+(\S+)\s+(\S+)\s+(\S+)')

def parse(f):
    d = {}
    for line in open(f):
        m = PAT.match(line)
        if not m: continue
        try: d[m.group(1).strip()] = {'wall': float(m.group(3)), 'gi': float(m.group(7)), 'gc': float(m.group(8))}
        except ValueError: pass
    return d

for pose in POSES:
    series = {}   # arm -> row -> col -> [values]
    for a in ARMS:
        s = {}
        for f in sorted(glob.glob(f'{OUT}/{a}_t{pose}_r*.txt')):
            if re.search(r'_r1\.txt$', f): continue
            for k, v in parse(f).items():
                s.setdefault(k, {'wall': [], 'gi': [], 'gc': []})
                for c in ('wall','gi','gc'): s[k][c].append(v[c])
        series[a] = s
    n = len(series[ARMS[0]].get('renderFrame', {}).get('gi', []))
    print(f'\n=== greets t={pose} — min over {n} kept rounds (r1 dropped) ===')
    print(f'{"row":<26}' + ''.join(f'{a:>14}' for a in ARMS) + '   floor(par)')
    for row in ROWS:
        for col, lbl in (('gi','Gi/f'), ('gc','Gcyc/f'), ('wall','wall')):
            vals = series[ARMS[0]].get(row, {}).get(col)
            if not vals: continue
            base = min(vals)
            floor = 100.0*(max(vals)-min(vals))/min(vals) if min(vals) else 0.0
            line = f'{row[:21]+" "+lbl:<26}'
            for a in ARMS:
                v = series[a].get(row, {}).get(col)
                if not v: line += f'{"-":>14}'; continue
                m = min(v)
                if a == ARMS[0]: line += f'{m:>14.4f}'
                else: line += f'{m:>9.4f}{100.0*(m-base)/base:+5.2f}%'.rjust(14)
            print(line + f'   {floor:5.2f}%')
        print()
