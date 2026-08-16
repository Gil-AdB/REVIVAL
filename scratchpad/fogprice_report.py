#!/usr/bin/env python3
"""min-of-rounds report for fogprice_ab.sh (round 0 dropped).

usage: fogprice_report.py <outdir> <poses,csv> <arms,csv> [rows,csv]
Prints, per pose, min wall_min and min Ginstr/f per arm for each DPROF phase,
with the delta of every arm against the FIRST one.
"""
import sys, re, glob

OUT = sys.argv[1]
POSES = sys.argv[2].split(',')
ARMS = sys.argv[3].split(',')
ROWS = (sys.argv[4].split(',') if len(sys.argv) > 4 else
        ['renderFrame', 'fastfog', 'fog-glow', 'fog-columns', 'fog-composite',
         'fog-skypaint', 'lighting-w1', 'DeferredLighting-call', 'gbuffer',
         'cones-call', 'TBR-render', 'water-ripple', 'water-glints'])


def num(t):
    try:
        return float(t)
    except ValueError:
        return None


def parse(path):
    """name -> (wall_min, Ginstr/f).  DPROF row layout:
       [DPROF] <name>  calls/f  wall_min  wall_avg  thrsum  effPar | Gi  Gc  IPC"""
    d = {}
    for line in open(path, errors='replace'):
        line = line.rstrip()
        if line.startswith('[DPROF]'):
            body = line[len('[DPROF]'):]
            if '|' not in body:
                continue
            left, right = body.split('|', 1)
            lt = left.split()
            rt = right.split()
            if len(lt) < 4:
                continue
            # trailing numeric/dash block on the left is 5 wide (calls..effPar)
            tail = lt[-5:]
            name = ' '.join(lt[:-5]).strip()
            if not name or 'OTHER' in name:
                continue
            wall = num(tail[1])
            gi = num(rt[0]) if rt else None
            if wall is None:
                continue
            d[name] = (wall, gi)
        elif line.startswith('TOTL'):
            t = line.split()
            if len(t) > 1 and num(t[1]) is not None:
                d['TOTL'] = (float(t[1]), None)
        elif line.startswith('slowest frames'):
            m = re.search(r':\s+([\d.]+)@', line)
            if m:
                d['frame-min'] = (float(m.group(1)), None)
    return d


def collect(arm, pose):
    vals = {}
    n = 0
    for f in sorted(glob.glob(f'{OUT}/{arm}_t{pose}_r*.txt')):
        r = int(re.search(r'_r(\d+)\.txt$', f).group(1))
        if r == 0:
            continue                     # warm-up round, always dropped
        n += 1
        for k, (w, g) in parse(f).items():
            a, b = vals.setdefault(k, ([], []))
            a.append(w)
            if g is not None:
                b.append(g)
    return vals, n


for pose in POSES:
    data, nr = {}, {}
    for a in ARMS:
        data[a], nr[a] = collect(a, pose)
    print(f'\n=== city t={pose}   (rounds used: ' +
          ', '.join(f'{a}={nr[a]}' for a in ARMS) + ') ===')
    print(f'{"row":30s}' + ''.join(f'{a:>12s}' for a in ARMS) +
          '   |' + ''.join(f'{("d "+a):>11s}' for a in ARMS[1:]))
    for row in ['frame-min', 'TOTL'] + ROWS:
        for col, idx in (('ms', 0), ('Gi', 1)):
            xs = []
            for a in ARMS:
                v = data[a].get(row)
                xs.append(min(v[idx]) if (v and v[idx]) else None)
            if xs[0] is None:
                continue
            line = f'{row+" "+col:30s}' + ''.join(
                (f'{x:12.4f}' if x is not None else f'{"-":>12s}') for x in xs)
            line += '   |' + ''.join(
                (f'{100.0*(x-xs[0])/xs[0]:+10.2f}%' if x is not None else f'{"-":>11s}')
                for x in xs[1:])
            print(line)
