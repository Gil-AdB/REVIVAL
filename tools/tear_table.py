#!/usr/bin/env python3
# tear_table.py <rawroot> <outdir> [arm-regex]  — runs tear_detect on every
# <arm>_<pose> dir that has a matching bare_<pose>, prints a table sorted by
# tear count and writes tears_table.txt.
import sys, os, re, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tear_detect as td
root, outdir = sys.argv[1], sys.argv[2]
armre = re.compile(sys.argv[3]) if len(sys.argv) > 3 else re.compile('^def$')
metrics = os.path.join(outdir, 'metrics_tears.txt')
rows = []
for d in sorted(os.listdir(root)):
    m = re.match(r'^(.+?)_([A-Z][A-Za-z0-9]*)$', d)
    if not m: continue
    arm, pose = m.group(1), m.group(2)
    if not armre.match(arm) or arm == 'bare': continue
    bare = os.path.join(root, 'bare_' + pose)
    if not os.path.isdir(bare) or not glob.glob(os.path.join(root, d, '*_refplane.txt')): continue
    try:
        n = td.detect(os.path.join(root, d), bare, d, outdir, metrics)
    except Exception as e:
        print('FAIL', d, e); continue
    rows.append((n['tear'], d, n))
rows.sort(reverse=True)
with open(os.path.join(outdir, 'tears_table.txt'), 'a') as f:
    f.write('--- %s ---\n' % ' '.join(sys.argv[1:]))
    for t, d, n in rows:
        line = '%-22s tear %6d  hole %5d see %5d black %4d | base %5d seam %5d panel %5d' % (
            d, t, n['hole'], n['see'], n['black'], n['base'], n['seam'], n['panel'])
        print(line); f.write(line + '\n')
