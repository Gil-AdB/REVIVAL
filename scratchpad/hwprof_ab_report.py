#!/usr/bin/env python3
"""Aggregate the raw [DPROF] tables hwprof_ab.sh saved, into an A/B report.

Separate from the runner on purpose: the runs are minutes long and the machine
is shared, so a reporting bug must never cost a re-measurement.

  usage: hwprof_ab_report.py <dir-of-rN_A.txt/rN_B.txt> [phase ...]
"""
import sys, os, re, glob, statistics

d = sys.argv[1]
want = sys.argv[2:] or ["renderFrame", "gbuffer", "DeferredLighting-call",
                        "lighting-w1", "lighting-w2", "shadow-bake", "cones",
                        "bloom-chain", "tonemap-post", "TBR-render"]

# [DPROF]     lighting-w1   1.00  29.875  30.536  344.959  11.3 |  3.458  1.001  3.455
ROW = re.compile(r"^\[DPROF\]\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+"
                 r"(?:([\d.]+)|-)\s+(?:([\d.]+)|-)\s*\|\s*(?:([\d.]+)|-)\s+"
                 r"(?:([\d.]+)|-)\s+(?:([\d.]+)|-)")

samples = {}   # (arm, phase) -> dict of lists
for f in sorted(glob.glob(os.path.join(d, "r*_*.txt"))):
    arm = os.path.basename(f).split("_")[1].split(".")[0]
    for line in open(f):
        m = ROW.match(line)
        if not m:
            continue
        ph = m.group(1)
        if ph not in want:
            continue
        s = samples.setdefault((arm, ph), {"wall": [], "instr": [], "cyc": [], "ipc": []})
        s["wall"].append(float(m.group(3)))                    # wall_min (g2 = calls/f)
        if m.group(7): s["instr"].append(float(m.group(7)))    # Ginstr/f
        if m.group(8): s["cyc"].append(float(m.group(8)))      # Gcyc/f
        if m.group(9): s["ipc"].append(float(m.group(9)))      # IPC

def pct(a, b):
    return (b - a) / a * 100.0 if a else 0.0

print("\n%-24s %9s %9s %7s | %8s %8s %7s | %8s %8s %7s | %6s %6s %6s" % (
    "phase", "wall_A", "wall_B", "d%", "Ginstr_A", "Ginstr_B", "d%",
    "Gcyc_A", "Gcyc_B", "d%", "IPC_A", "IPC_B", "d%"))
print("-" * 130)
for ph in want:
    a = samples.get(("A", ph)); b = samples.get(("B", ph))
    if not a or not b or not a["wall"] or not b["wall"]:
        continue
    wa, wb = min(a["wall"]), min(b["wall"])          # wall: min over rounds
    row = "%-24s %9.3f %9.3f %+6.1f%%" % (ph, wa, wb, pct(wa, wb))
    for k, w in (("instr", 4), ("cyc", 4), ("ipc", 3)):
        if a[k] and b[k]:
            # counters: median over rounds (each is already a mean over the
            # run's steady frames; not a min-statistic)
            ma, mb = statistics.median(a[k]), statistics.median(b[k])
            fmt = " | %8.4f %8.4f %+6.1f%%" if w == 4 else " | %6.3f %6.3f %+6.1f%%"
            row += fmt % (ma, mb, pct(ma, mb))
        else:
            row += " | %8s %8s %7s" % ("-", "-", "-") if w == 4 else " | %6s %6s %6s" % ("-", "-", "-")
    print(row)
    n = len(a["wall"])
    if ph == "lighting-w1":
        print("   %-21s A wall samples: %s" % ("", " ".join("%.2f" % x for x in sorted(a["wall"]))))
        print("   %-21s B wall samples: %s" % ("", " ".join("%.2f" % x for x in sorted(b["wall"]))))
        if a["ipc"] and b["ipc"]:
            print("   %-21s A IPC samples : %s" % ("", " ".join("%.3f" % x for x in sorted(a["ipc"]))))
            print("   %-21s B IPC samples : %s" % ("", " ".join("%.3f" % x for x in sorted(b["ipc"]))))

print("\nd%% = B relative to A. Negative = B faster / fewer / lower.")
print("wall = min over rounds (ms/frame); Ginstr/Gcyc = median over rounds (billions/frame); IPC = median.")
print("IPC is the load-robust discriminator: a descheduled worker costs wall time but")
print("retires no instructions and burns no cycles. Prefer it when wall and IPC disagree.")
print("rounds parsed: A=%d B=%d" % (
    len(samples.get(("A", "renderFrame"), {"wall": []})["wall"]),
    len(samples.get(("B", "renderFrame"), {"wall": []})["wall"])))
