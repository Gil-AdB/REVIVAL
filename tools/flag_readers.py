#!/usr/bin/env python3
"""Standing check: every FDS_FLAG_* in FeatureFlags.def must have a READER.

    tools/flag_readers.py            exit 1 and list offenders
    tools/flag_readers.py --json     machine-readable
    tools/flag_readers.py --verbose  also print where each flag IS read

THE FAILURE THIS CATCHES: a flag lands with its .def row, its help text, its
CLI plumbing — and no code that ever asks for its value. It looks like a dial.
It is a decoration. (The C3 dial was exactly this.) The prose convention
"a flag must be read" is not enforceable by reading prose.

THE TRAP THIS AVOIDS: the tree spells a flag read FOUR ways, and a check that
knows only one of them reports confident nonsense.

    1. fds::FeatureFlags::name()      the generated accessor
    2. FF::name()                     the `using FF = fds::FeatureFlags` alias
    3. BoolId::name / FloatId::name / IntId::name
                                      get() / isSet() / setDefault()
    4. "name"                         the CLI-name string, for the param
                                      registry, the tune console, ParamScript,
                                      and REV.CPP's raw argv pre-scans

NEGATIVE CONTROL (re-run it if you touch the patterns): append a bogus
FDS_FLAG_BOOL row to FeatureFlags.def and this must exit 1 naming it. On
2026-08-30 it did, with the other 609 still passing — so "0 offenders" here is
a result, not a vacuous check.

A hit inside a COMMENT is not a reader. That distinction is not cosmetic: this
tree documents flags in enormous help strings and in block comments that name
other flags, so a naive grep credits a dead flag with dozens of "readers". The
comment/string blanking is reused from tools/flag_audit.py, which already got
it right, rather than reimplemented here.
"""

from __future__ import annotations

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flag_audit as fa  # noqa: E402

# The .def itself is definition, not use. flag_audit.py names flags in its own
# tables and would credit itself as a reader; so would this file's docstring.
SKIP_RELPATHS = {
    "FDS/Base/FeatureFlags.def",
    "tools/flag_audit.py",
    "tools/flag_readers.py",
}

# Where a reader may live. The flag plumbing (FeatureFlags.cpp/.h,
# ParamScript.cpp, TuneServer.cpp) is scanned like anything else: a name
# reachable from the tune console or a ParamScript IS reachable, and an earlier
# revision of this check demoted those hits to a second-class "REGISTRY-ONLY"
# tier that fired on flags a user can genuinely drive. The .def stays excluded
# because a row in it is the definition, not a use.
SCAN_DIRS = ["DEMO", "FDS", "tests", "GpuBench"]


def build_patterns(names):
    acc = {n: re.compile(r'(?:FeatureFlags|FF)::' + re.escape(n) + r'\s*\(') for n in names}
    idr = {n: re.compile(r'\b(?:Bool|Float|Int)Id::' + re.escape(n) + r'\b') for n in names}
    # CLI-name string: the bare name, or its --dash / --no-dash form, quoted.
    strr = {n: re.compile(r'"(?:--(?:no-)?)?' + re.escape(n).replace('_', '[_-]') + r'"')
            for n in names}
    return acc, idr, strr


def main(argv):
    as_json = "--json" in argv
    verbose = "--verbose" in argv

    flags, order = fa.parse_def()
    names = list(flags)
    acc, idr, strr = build_patterns(names)

    readers = {n: [] for n in names}          # (relpath, line, kind)
    for path in fa.walk_files(SCAN_DIRS, fa.SRC_EXT):
        rel = os.path.relpath(path, fa.ROOT)
        if rel in SKIP_RELPATHS:
            continue
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        here = [n for n in names if n in text]
        if not here:
            continue
        code = fa.blank(text, keep_strings=True)     # comments blanked, strings kept
        lines = text.split("\n")
        offs, o = [], 0
        for ln in lines:
            offs.append(o)
            o += len(ln) + 1
        for n in here:
            for rx, kind in ((acc[n], "acc"), (idr[n], "id"), (strr[n], "str")):
                for m in rx.finditer(code):
                    li = fa._line_of(offs, m.start())
                    readers[n].append((rel, li + 1, kind))

    offenders = [n for n in names if not readers[n]]

    if as_json:
        print(json.dumps({
            "total": len(names),
            "unread": offenders,
            "readers": {n: [{"file": f, "line": l, "kind": k}
                            for (f, l, k) in readers[n]] for n in names}
            if verbose else {},
        }, indent=2))
    else:
        print(f"[flag_readers] {len(names)} flags in FeatureFlags.def, "
              f"{len(names) - len(offenders)} with a reader")
        for n in offenders:
            f = flags[n]
            print(f"  NO READER      {n:38s} ({f['type']}, {f['category']}) "
                  f"line {f['lineno']}")
        if verbose:
            for n in names:
                if readers[n]:
                    print(f"  {n}: " + ", ".join(f"{f}:{l}({k})" for f, l, k in readers[n][:6]))

    if offenders:
        print(f"[flag_readers] FAIL: {len(offenders)} flag(s) read by nothing "
              f"in any of the four spellings", file=sys.stderr)
        return 1
    print("[flag_readers] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
