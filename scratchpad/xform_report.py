import sys, collections
# min-of-N per (section, arm, column) + noise floor = max over arms of (2nd-min - min)/min
COLS = ["framemin","totl","rf_wall","rf_ginstr","rf_gcyc","gbuf_ginstr","rndr","xfrm","anim"]
sec = None
data = collections.OrderedDict()
for path in sys.argv[1:]:
    for ln in open(path):
        ln = ln.strip()
        if not ln: continue
        if ln.startswith("####"):
            sec = ln[4:].strip(); data.setdefault(sec, collections.OrderedDict()); continue
        p = ln.split(",")
        if len(p) < 3 or sec is None: continue
        arm = p[1]
        if arm == "--": continue
        vals = p[2:2+len(COLS)]
        row = data[sec].setdefault(arm, collections.defaultdict(list))
        for c, v in zip(COLS, vals):
            try: row[c].append(float(v))
            except: pass
for sec, arms in data.items():
    print("=== " + sec)
    print("  %-46s %s" % ("arm", "  ".join("%10s" % c for c in COLS)))
    floors = {}
    for c in COLS:
        f = 0.0
        for a, row in arms.items():
            v = sorted(row.get(c, []))
            if len(v) >= 2 and v[0] > 0: f = max(f, (v[1]-v[0])/v[0]*100)
        floors[c] = f
    base = None
    for a, row in arms.items():
        mins = {}
        for c in COLS:
            v = sorted(row.get(c, []))
            mins[c] = v[0] if v else float('nan')
        if base is None: base = mins
        line = "  %-46s " % (a + " (n=%d)" % len(row.get("framemin", [])))
        for c in COLS:
            d = ""
            if base is not mins and base.get(c):
                d = " %+.2f%%" % ((mins[c]-base[c])/base[c]*100)
            line += "%10.3f%-8s" % (mins[c], d)
        print(line)
    print("  %-46s %s" % ("noise floor %", "  ".join("%9.2f " % floors[c] for c in COLS)))
