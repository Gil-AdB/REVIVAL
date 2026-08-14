#!/usr/bin/env python3
"""prof1_symbols.py — per-SYMBOL attribution via Instruments' Time Profiler.

`--deferred_prof` says which PHASE is slow; this says which FUNCTION inside it.
Recording is unprivileged and headless (SDL dummy drivers), so it can run in an
agent session.

  prof1_symbols.py record  OUT.trace -- ./DEMO <args...>
  prof1_symbols.py report  OUT.trace [topN]
  prof1_symbols.py both    OUT.trace [topN] -- ./DEMO <args...>

Two traps, both handled here rather than re-derived:

* **xcrun xctrace does not work; the absolute path does.** Selection problem,
  not availability — `DEVELOPER_DIR` plus the Xcode.app path is the fix.
* **The XML export id/ref trap.** Instruments emits each repeated value ONCE
  with an `id=` attribute and every later occurrence as an empty element with
  `ref=`. A naive parse reads those as blank and loses most of the weight.
  `resolve()` below keeps an id table and substitutes.

Weight is `sample-time`-weighted "running" samples, i.e. SELF time, which is the
column that answers "what is the CPU actually executing".
"""
import os, subprocess, sys, xml.etree.ElementTree as ET
from collections import defaultdict

XCTRACE = "/Applications/Xcode.app/Contents/Developer/usr/bin/xctrace"
DEVDIR = "/Applications/Xcode.app/Contents/Developer"
ENV = dict(os.environ, DEVELOPER_DIR=DEVDIR,
           SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")


def record(trace, argv):
    if os.path.exists(trace):
        subprocess.run(["rm", "-rf", trace])
    cmd = [XCTRACE, "record", "--template", "Time Profiler", "--output", trace,
           "--target-stdout", "/dev/null", "--launch", "--"] + argv
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True, timeout=3600)
    sys.stderr.write(p.stdout[-2000:] + p.stderr[-2000:])
    return p.returncode


def export(trace, xpath):
    p = subprocess.run([XCTRACE, "export", "--input", trace, "--xpath", xpath],
                       env=ENV, capture_output=True, text=True, timeout=3600)
    if p.returncode != 0:
        sys.stderr.write(p.stderr[-3000:])
        return None
    return p.stdout


def report(trace, topn=30, binary_filter=None):
    xml = export(trace, '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]')
    if xml is None:
        print(export(trace, "/trace-toc") or "(no toc)")
        return
    root = ET.fromstring(xml)

    # THE ID/REF TRAP. Instruments writes each distinct value ONCE carrying an
    # id=, and every later occurrence as an EMPTY element with ref=. A parser
    # that reads element text loses most of its weight to blanks. Build the id
    # table first, then dereference. Frames and binaries need it independently
    # because a frame's <binary> is itself usually a ref.
    ids = {}
    for el in root.iter():
        i = el.get("id")
        if i is not None:
            ids[i] = el

    def deref(el):
        r = el.get("ref")
        return ids.get(r, el) if r is not None else el

    per_sym, per_bin, per_thread = defaultdict(float), defaultdict(float), defaultdict(float)
    total = 0.0
    for row in root.iter("row"):
        w, sym, binr, thr, state = 0.0, None, None, None, None
        for child in row:
            c = deref(child)
            tag = c.tag
            if tag == "weight":
                w = float(c.text or 0) / 1e6          # ns -> ms
            elif tag == "thread":
                thr = c.get("fmt")
            elif tag == "thread-state":
                state = c.get("fmt") or c.text
            elif tag == "tagged-backtrace":
                bt = c.find("backtrace")
                if bt is None:
                    continue
                fr = list(bt)
                if not fr:
                    continue
                f = deref(fr[0])                       # frame 0 = the LEAF = self
                sym = f.get("name")
                b = f.find("binary")
                if b is not None:
                    binr = deref(b).get("name")
        if state and state != "Running":
            continue                                   # self time = running samples only
        if binary_filter and binr != binary_filter:
            continue
        total += w
        per_sym[(binr, sym)] += w
        per_bin[binr] += w
        per_thread[thr] += w
    if total == 0:
        print("no running samples parsed")
        return
    print("# total running weight: %.1f ms%s" % (total, " (binary=%s)" % binary_filter if binary_filter else ""))
    print("\n== per binary ==")
    for b, v in sorted(per_bin.items(), key=lambda x: -x[1])[:8]:
        print("  %7.2f%%  %10.1f ms  %s" % (100 * v / total, v, b))
    print("\n== per symbol (SELF) ==")
    for (b, s), v in sorted(per_sym.items(), key=lambda x: -x[1])[:topn]:
        print("  %7.2f%%  %10.1f ms  %-10s %s" % (100 * v / total, v, (b or "?")[:10], (s or "?")[:96]))


if __name__ == "__main__":
    mode = sys.argv[1]
    trace = sys.argv[2]
    if "--" in sys.argv:
        argv = sys.argv[sys.argv.index("--") + 1:]
    else:
        argv = []
    topn = 30
    for a in sys.argv[3:]:
        if a.isdigit():
            topn = int(a)
            break
    if mode in ("record", "both"):
        rc = record(trace, argv)
        print("# xctrace record rc=%d" % rc)
    if mode in ("report", "both"):
        bf = None
        if "--binary" in sys.argv:
            bf = sys.argv[sys.argv.index("--binary") + 1]
        report(trace, topn, bf)
