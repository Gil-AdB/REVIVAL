#!/usr/bin/env python3
"""Authoring-source archaeology: test whether an LWS candidate (plus a chosen
set of LWO objects) converts byte-identical to a shipping FLD.

  tools/pin_scene.py <candidate.LWS> <shipping.FLD> [--pick obj=path ...]

Stages the LWS + every object it references into a temp dir (lwsread resolves
DOS object paths by basename in CWD), runs tools/lwsread, and reports:
  - missing objects (basename not found anywhere under Original/)
  - ambiguous objects (multiple distinct copies — each md5'd so variants can
    be --pick'ed on a later run)
  - output size vs shipping size, and the first byte-difference offset
Exit 0 only on byte-identical output.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LWSREAD = os.path.join(REPO, "tools", "lwsread", "build", "lwsread")
LWSREAD_LEGACY = os.path.join(REPO, "tools", "lwsread", "build", "lwsread_legacy")
LWSREAD_OFIR = os.path.join(REPO, "tools", "lwsread", "build", "lwsread_ofir")
ORIGINAL = os.path.join(REPO, "Original")


def md5(p, n=1 << 30):
    h = hashlib.md5()
    with open(p, "rb") as f:
        h.update(f.read(n))
    return h.hexdigest()[:10]


def find_all(basename):
    """Every file under Original/ whose basename matches (case-insensitive),
    deduped by content hash."""
    hits = {}
    want = basename.lower()
    for root, _dirs, files in os.walk(ORIGINAL):
        for f in files:
            if f.lower() == want:
                p = os.path.join(root, f)
                hits.setdefault(md5(p), []).append(p)
    return hits


def lws_objects(path):
    """Basenames of every LoadObject in the LWS (DOS paths stripped)."""
    objs = []
    for line in open(path, encoding="latin-1"):
        m = re.match(r"\s*LoadObject\s+(.+?)\s*$", line)
        if m:
            objs.append(m.group(1).replace("\\", "/").split("/")[-1])
    return objs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lws")
    ap.add_argument("fld")
    ap.add_argument("--pick", action="append", default=[],
                    help="objname.lwo=/explicit/path — resolve an ambiguous object")
    ap.add_argument("--legacy-vlum", action="store_true",
                    help="use lwsread_legacy (VLUM*100 behavior) for 1998 shipping FLDs")
    ap.add_argument("--ofir", action="store_true",
                    help="use lwsread_ofir (VLUM*100 + LastFrame keyword) for OFIR-era FLDs")
    args = ap.parse_args()
    picks = dict(p.split("=", 1) for p in args.pick)
    lwsread_bin = LWSREAD_OFIR if args.ofir else (LWSREAD_LEGACY if args.legacy_vlum else LWSREAD)

    objs = lws_objects(args.lws)
    print(f"[lws] {args.lws}: {len(objs)} object refs")

    tmp = tempfile.mkdtemp(prefix="pinscene_")
    staged, missing, ambiguous = [], [], []
    for o in dict.fromkeys(objs):   # dedupe, keep order
        if o in picks:
            shutil.copy2(picks[o], os.path.join(tmp, o))
            staged.append((o, picks[o]))
            continue
        hits = find_all(o)
        if not hits:
            missing.append(o)
            continue
        if len(hits) > 1:
            ambiguous.append((o, hits))
        # default: first hash group, first path (stable order for iteration)
        first = sorted(hits.items())[0][1][0]
        shutil.copy2(first, os.path.join(tmp, o))
        staged.append((o, first))

    for o in missing:
        print(f"  MISSING {o}")
    for o, hits in ambiguous:
        print(f"  AMBIGUOUS {o}: {len(hits)} distinct copies")
        for h, paths in sorted(hits.items()):
            print(f"    {h}  {paths[0]}" + (f"  (+{len(paths)-1} dups)" if len(paths) > 1 else ""))
    if missing:
        print("[pin] cannot convert — objects missing")
        return 2

    lws_local = os.path.join(tmp, os.path.basename(args.lws))
    shutil.copy2(args.lws, lws_local)
    out = os.path.join(tmp, "out.fld")
    r = subprocess.run([lwsread_bin, os.path.basename(lws_local), "out.fld"],
                       cwd=tmp, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        print(f"[lwsread] FAILED rc={r.returncode}\n{(r.stderr or r.stdout)[-1500:]}")
        return 3

    got, want = os.path.getsize(out), os.path.getsize(args.fld)
    print(f"[size] out={got} shipping={want} delta={got-want}")
    a, b = open(out, "rb").read(), open(args.fld, "rb").read()
    if a == b:
        print(f"[PIN] BYTE-IDENTICAL — staged set in {tmp}")
        for o, p in staged:
            print(f"   {o} <- {p}")
        return 0
    n = min(len(a), len(b))
    off = next((i for i in range(n) if a[i] != b[i]), n)
    print(f"[diff] first byte difference at offset {off} (0x{off:x}) of {n}")
    print(f"[tmp] staged set kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
