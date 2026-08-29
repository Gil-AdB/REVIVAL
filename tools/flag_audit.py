#!/usr/bin/env python3
"""FeatureFlags.def cull audit — mechanical, read-only, reproducible.

Usage:  python3 tools/flag_audit.py [--json OUT.json] [--md OUT.md]

Produces, per flag in FDS/Base/FeatureFlags.def:
  name, type, env, default, category, help-text length,
  keyword hits in the help text (REFUTED / superseded / instrument / ...),
  code references OUTSIDE the .def (accessor calls, *Id:: refs, env-var
  string, CLI token in scripts/docs),
  whether any code reference sits inside a for/while loop body,
  git history of the flag's row (introduced, last touched, default history),
  groundwork ledger records whose subject/claim names the flag,
  and a bucket assignment.

Everything is derived from the tree; nothing is hand-entered.
"""
import argparse, json, os, re, subprocess, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF  = os.path.join(ROOT, "FDS/Base/FeatureFlags.def")

# ── 1. parse the .def ────────────────────────────────────────────────────
FLAG_RE = re.compile(r'^FDS_FLAG_(BOOL|FLOAT|INT)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,')

KEYWORDS = {
    "REFUTED":     re.compile(r'\bREFUTED\b'),
    "refuted":     re.compile(r'\brefuted\b'),
    "superseded":  re.compile(r'supersed', re.I),
    "instrument":  re.compile(r'instrument', re.I),
    "A/B only":    re.compile(r'A/B[- ]only', re.I),
    "his eye":     re.compile(r'his eye', re.I),
    "default OFF": re.compile(r'default[s]? OFF', re.I),
    "CANDIDATE":   re.compile(r'\bCANDIDATE\b'),
    "byte-exact":  re.compile(r'byte-(exact|identical|null)', re.I),
    "DEAD":        re.compile(r'\bDEAD\b'),
    "no longer":   re.compile(r'no longer', re.I),
}

def split_args(s):
    """Split a macro arg list on top-level commas (respects () and \"\")."""
    out, depth, cur, instr, esc = [], 0, [], False, False
    for ch in s:
        if esc:
            cur.append(ch); esc = False; continue
        if ch == '\\':
            cur.append(ch); esc = True; continue
        if instr:
            cur.append(ch)
            if ch == '"': instr = False
            continue
        if ch == '"': instr = True; cur.append(ch); continue
        if ch in '([': depth += 1
        elif ch in ')]': depth -= 1
        if ch == ',' and depth == 0:
            out.append(''.join(cur)); cur = []
        else:
            cur.append(ch)
    out.append(''.join(cur))
    return out

def parse_def():
    flags = {}
    order = []
    section = ""
    dev_depth = 0
    with open(DEF, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            st = line.strip()
            if st.startswith("#if"):
                dev_depth += 1; continue
            if st.startswith("#endif"):
                dev_depth = max(0, dev_depth - 1); continue
            m = re.match(r'^//\s*[-─]{2,}\s*(.*?)\s*[-─]{2,}', st)
            if m:
                section = m.group(1); continue
            m = FLAG_RE.match(line)
            if not m:
                continue
            typ, name = m.group(1), m.group(2)
            body = line[line.index('(') + 1:].rstrip()
            body = body[:body.rfind(')')]
            parts = split_args(body)
            env  = parts[1].strip().strip('"') if len(parts) > 1 else ""
            dflt = parts[2].strip()            if len(parts) > 2 else ""
            cat  = parts[3].strip().strip('"') if len(parts) > 3 else ""
            help_ = ','.join(parts[4:]).strip() if len(parts) > 4 else ""
            help_ = help_.strip()
            if help_.startswith('"'): help_ = help_[1:]
            if help_.endswith('"'):   help_ = help_[:-1]
            flags[name] = dict(
                name=name, type=typ, env=env, default=dflt, category=cat,
                section=section, lineno=lineno, help=help_, help_len=len(help_),
                line_len=len(line), dev_only=bool(dev_depth),
                kw=sorted(k for k, r in KEYWORDS.items() if r.search(help_)),
            )
            order.append(name)
    return flags, order

# ── 2. reference scan ────────────────────────────────────────────────────
SRC_EXT  = {'.cpp', '.h', '.hpp', '.c', '.CPP', '.H', '.cc', '.inl', '.def'}
TXT_EXT  = {'.py', '.sh', '.md', '.params', '.txt', '.json', '.cfg', '.html', '.js'}
SKIP_DIRS = {'simd', 'simde', 'Original', 'build', '.git', 'node_modules',
             '__pycache__', 'modplayer', 'WASMEXE'}

def walk_files(dirs, exts):
    for d in dirs:
        base = os.path.join(ROOT, d)
        if os.path.isfile(base):
            yield base; continue
        for dp, dns, fns in os.walk(base):
            dns[:] = [x for x in dns if x not in SKIP_DIRS]
            for fn in fns:
                if os.path.splitext(fn)[1] in exts:
                    yield os.path.join(dp, fn)

def blank(text, keep_strings):
    """Return `text` with comments blanked to spaces (newlines kept).
    If keep_strings is False, string/char literals are blanked too."""
    buf = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i+1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j): buf[k] = ' '
            i = j
        elif c == '/' and i + 1 < n and text[i+1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, min(j, n)):
                if buf[k] != '\n': buf[k] = ' '
            i = j
        elif c in '"\'':
            q, j = c, i + 1
            while j < n:
                if text[j] == '\\': j += 2; continue
                if text[j] == q: j += 1; break
                if text[j] == '\n' and q == "'": break
                j += 1
            if not keep_strings:
                for k in range(i, min(j, n)):
                    if buf[k] != '\n': buf[k] = ' '
            i = min(j, n) if j > i else i + 1
        else:
            i += 1
    return ''.join(buf)

def loop_scopes(clean):
    """Number of enclosing for/while/do bodies at each character offset of
    `clean` (comments AND string literals already blanked).

    Brace-depth walk; a `for`/`while`/`do` arms the next `{`. Brace-less
    single-statement loop bodies are NOT tracked (documented limitation:
    they under-count, never over-count)."""
    kw = re.compile(r'\b(for|while|do)\s*[\(\{]')
    pending = [m.start() for m in kw.finditer(clean)]
    depth_at = [0] * (len(clean) + 1)
    depth = 0
    loop_depths = []
    pi = 0
    armed = False
    paren = 0
    for idx, ch in enumerate(clean):
        while pi < len(pending) and pending[pi] <= idx:
            armed = True; pi += 1
        depth_at[idx] = len(loop_depths)
        if ch == '(':
            paren += 1
        elif ch == ')':
            paren = max(0, paren - 1)
        elif ch == '{':
            depth += 1
            if armed and paren == 0:
                loop_depths.append(depth); armed = False
        elif ch == '}':
            if loop_depths and loop_depths[-1] == depth:
                loop_depths.pop()
            depth -= 1
        elif ch == ';' and armed and paren == 0:
            # a brace-less loop body ended (`for (...) foo();`) -- the loop
            # never opened a scope, so nothing to push. NOT counted (this is
            # the detector's one documented under-count).
            armed = False
    depth_at[len(clean)] = len(loop_depths)
    return depth_at

HOT_DIR = re.compile(r'FDS/(RENDER|FILLERS|FRUSTRUM)/|FDS/Clipper\.cpp')

def scan_refs(flags):
    """Count references to each flag OUTSIDE FeatureFlags.def.

    Four reference kinds, because the codebase uses four spellings:
      acc  — fds::FeatureFlags::<name>() / FF::<name>()   (the generated accessor)
      id   — (Bool|Float|Int)Id::<name>                   (get/isSet/setDefault)
      str  — the bare name as a C string literal          (findBoolByCliName,
                                                            setParamFromText, ...)
      argv — "--<name>" / "--no-<name>" string literal    (REV.CPP raw argv scan)
    Matches found in comments are counted separately and NEVER as code refs.
    """
    acc  = {n: re.compile(r'(?:FeatureFlags|FF)::' + re.escape(n) + r'\s*\(') for n in flags}
    idr  = {n: re.compile(r'\b(?:Bool|Float|Int)Id::' + re.escape(n) + r'\b') for n in flags}
    strr = {n: re.compile(r'"(?:--(?:no-)?)?' + re.escape(n) + r'"') for n in flags}
    envr = {n: re.compile(r'"' + re.escape(f['env']) + r'"') for n, f in flags.items() if f['env']}
    clir = {n: re.compile(r'--(?:no-)?' + re.escape(n).replace('_', '[_-]') + r'\b') for n in flags}

    refs = defaultdict(lambda: dict(code=[], comment=[], inloop=[], env=[], cli=[]))
    for path in walk_files(['DEMO', 'FDS', 'tools'], SRC_EXT):
        rel = os.path.relpath(path, ROOT)
        if rel == 'FDS/Base/FeatureFlags.def':
            continue
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        names_here = [n for n in flags if n in text]
        if not names_here:
            continue
        nocomment = blank(text, keep_strings=True)    # code + string literals
        nostring  = blank(text, keep_strings=False)   # code only
        depth_at  = loop_scopes(nostring)
        lines = text.split('\n')
        offs, o = [], 0
        for ln in lines:
            offs.append(o); o += len(ln) + 1
        hot = bool(HOT_DIR.search(rel))
        for n in names_here:
            for rx, kind, view in ((acc[n], 'acc', nocomment),
                                   (idr[n], 'id', nocomment),
                                   (strr[n], 'str', nocomment)):
                for m in rx.finditer(view):
                    li = _line_of(offs, m.start())
                    refs[n]['code'].append(dict(
                        file=rel, line=li + 1, kind=kind,
                        loop=depth_at[m.start()], hot=hot,
                        text=lines[li].strip()[:180]))
            # comment-only mentions (in raw text but not in the code view)
            raw_n = len(re.findall(r'(?:FeatureFlags|FF)::' + re.escape(n) + r'\s*\(', text))
            code_n = len([x for x in refs[n]['code'] if x['kind'] == 'acc'])
            if raw_n > code_n:
                refs[n]['comment'].append(dict(file=rel, n=raw_n - code_n))
        for x in refs_inloop_iter(refs, names_here):
            pass
    for n in refs:
        refs[n]['inloop'] = [c for c in refs[n]['code'] if c['loop'] > 0 and c['kind'] != 'str']

    # env-var name and CLI-token mentions anywhere in the tree (code, scripts,
    # docs, Runtime) — evidence a flag is driven from outside C++.
    for path in walk_files(['DEMO', 'FDS', 'tools', 'docs', 'Runtime', 'Scenes'],
                           SRC_EXT | TXT_EXT):
        rel = os.path.relpath(path, ROOT)
        # Skip the .def itself and THIS SCRIPT -- flag_audit.py names flags in
        # its own overrides table and would otherwise credit itself as a user.
        if rel in ('FDS/Base/FeatureFlags.def', 'tools/flag_audit.py'):
            continue
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for n, rx in envr.items():
            if rx.search(text):
                refs[n]['env'].append(rel)
        for n, rx in clir.items():
            if len(n) < 4:
                continue
            if rx.search(text):
                refs[n]['cli'].append(rel)
    return refs

def refs_inloop_iter(refs, names):
    return ()

def _line_of(offs, pos):
    lo, hi = 0, len(offs) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if offs[mid] <= pos: lo = mid
        else: hi = mid - 1
    return lo

# ── 3. git history of each flag row ──────────────────────────────────────
SEP = '@@@FLAGAUDIT@@@'

def git_history(flags):
    out = subprocess.run(
        ['git', '-C', ROOT, 'log', '--no-merges', '--date=iso',
         f'--format={SEP}%H|%cd|%s%n%b{SEP}BODYEND', '-p', '--unified=0',
         '--', 'FDS/Base/FeatureFlags.def'],
        capture_output=True, text=True, errors='replace').stdout
    hist = defaultdict(list)     # name -> [(sha, date, subj, body, added_default)]
    chunks = out.split(SEP)
    i = 1
    entries = []
    while i < len(chunks):
        head = chunks[i]
        sha, date, rest = head.split('|', 2)
        subj_body = rest
        i += 1
        diff = chunks[i] if i < len(chunks) else ''
        if diff.startswith('BODYEND'):
            diff = diff[len('BODYEND'):]
        i += 1
        entries.append((sha, date, subj_body, diff))
    for sha, date, msg, diff in entries:
        seen = {}
        for ln in diff.split('\n'):
            if not ln or ln[0] not in '+-':
                continue
            if ln.startswith('+++') or ln.startswith('---'):
                continue
            m = re.match(r'^[+-]FDS_FLAG_(BOOL|FLOAT|INT)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,', ln)
            if not m:
                continue
            name = m.group(2)
            if name not in flags:
                continue
            rec = seen.setdefault(name, dict(added=None, removed=None))
            if ln[0] == '+':
                parts = split_args(ln[ln.index('(') + 1:])
                rec['added'] = parts[2].strip() if len(parts) > 2 else None
            else:
                parts = split_args(ln[ln.index('(') + 1:])
                rec['removed'] = parts[2].strip() if len(parts) > 2 else None
        for name, rec in seen.items():
            hist[name].append(dict(sha=sha[:8], date=date[:10], msg=msg.strip(),
                                   added_default=rec['added'],
                                   removed_default=rec['removed']))
    return hist

PROOF_RE = re.compile(r'byte-exact|byte-identical|byte-null|\bmd5\b|bit-exact|bit-identical', re.I)

# ── 4. groundwork ledger ────────────────────────────────────────────────
def groundwork(flags):
    gw = os.environ.get('GROUNDWORK_BIN',
                        '/Users/gil-ad/work/groundwork/.venv/bin/groundwork')
    env = dict(os.environ, GROUNDWORK_WRITER='subagent:flagaudit')
    try:
        raw = subprocess.run([gw, 'export', '--json'], cwd=ROOT, env=env,
                             capture_output=True, text=True).stdout
        recs = json.loads(raw)
    except Exception as e:
        print(f'[flag_audit] groundwork unavailable: {e}', file=sys.stderr)
        return {}
    hits = defaultdict(list)
    for r in recs:
        blob = (r.get('subject', '') + ' ' + r.get('claim', '') + ' ' +
                json.dumps(r.get('scope', {})))
        for n in flags:
            if len(n) < 5:
                continue
            if re.search(r'\b' + re.escape(n) + r'\b', blob):
                hits[n].append(dict(id=r.get('id'), kind=r.get('kind'),
                                    subject=r.get('subject'),
                                    claim=r.get('claim', '')[:400]))
    return hits

# ── 5. bucketing ────────────────────────────────────────────────────────
# The audit date and the "default ON for >= 2 weeks" cut-off. Both are
# parameters, not constants, so the run is reproducible on any day.
AUDIT_DATE   = os.environ.get('FLAG_AUDIT_DATE', '2026-08-29')
LANDED_CUTOFF = os.environ.get('FLAG_AUDIT_LANDED_CUTOFF', '2026-08-15')  # >= 14 d

RE_LOOK_PENDING = re.compile(
    r'CHANGES THE (SHIPPING LOOK|AMBIENT LEVEL)|pending the user|his eye|owner\'s eye|'
    r'his call|look call|LOOK DECISION|awaiting a call|unjudged look', re.I)
RE_BYTE_POS = re.compile(
    r'(?<!not )(byte-null|byte-identical|byte-exact|bit-exact|bit-identical)', re.I)
RE_BYTE_NEG = re.compile(r'\bnot (byte-null|byte-identical|byte-exact|bit-exact)', re.I)
RE_INSTRUMENT = re.compile(r'^\s*\[(INSTRUMENT|VIZ|DEBUG|DUMP|CENSUS|DIAGNOSTIC)', re.I)
RE_INSTR_NAME = re.compile(
    r'(^|_)(viz|dump|census|stats|verify|hash|log|probe_log|prof|trace|report|assert)($|_)')
RE_SUPERSEDED = re.compile(r'supersed|no longer (used|read|wired|fires)|\bDEAD\b')

def noop_default(f):
    """True when the flag's compile default is its inert / no-op value."""
    d = f['default'].strip()
    if f['type'] == 'BOOL':
        return d == '0' or d.endswith('_DEFAULT_ON') and False
    return d in ('0', '0.0f', '0.f', '-1')

def on_since(f):
    """(date, sha) at which the default last became 1, or None."""
    out = None
    for d, sha, added in f['default_history']:
        if added == '1':
            out = (d, sha)
        elif added is None:
            continue
        else:
            break
    return out

def gw_refutes(f):
    return [g for g in f['gw'] if g['kind'] == 'refutation'
            and (g['subject'] == 'flag.' + f['name']
                 or g['subject'].startswith('flag.' + f['name'] + '.'))]

def gw_open_menu(f):
    return [g for g in f['gw'] if g['kind'] == 'open'
            and ('menu.' + f['name'] in g['subject'] or f['name'] in g['subject'])]

# Hand-adjudicated rows. Every entry is a row whose HELP TEXT defeats the
# mechanical rule -- the byte-equality phrase is there but is not a claim about
# THIS flag's two arms. Each carries the sentence that forced the override, so
# the decision is auditable rather than a magic list.
MANUAL_OVERRIDES = {
    'strict_flags': ('KEEP-TUNABLE',
        "the 'byte-identical' phrase is a HISTORICAL ANECDOTE about a 2026-08-08 "
        "incident, not a claim about this flag's arms; --no-strict_flags is a "
        "documented escape hatch for scripts that pass foreign argv words."),
    'pom_shell_base_clip': ('UNSURE',
        "the byte claim is 'Inert without --pom_shell, so flags-off is "
        "byte-identical' -- about the PARENT gate (--pom_shell, default 0), not "
        "about this flag's arms; live A/B in the S1d closed-shell research."),
    'pom_prism_flat': ('UNSURE',
        "the byte claim is 'inert without --pom_prism (itself default 0), so "
        "shipping stays byte-null' -- about the PARENT gate, not the arms."),
    'greets_displace_border_pin': ('UNSURE',
        "its own text: 'it PRICES the crack safety, it is not a fix' -- an "
        "explicit pricing knob for the live displacement research, not a landed "
        "lever with a dead arm."),
    'viz_legend': ('KEEP-DEBUG',
        "gates the on-screen legend for the debug vizzes (--pom_path_viz, "
        "--wire_viz, --displace_viz, --pom_seam_viz); 'byte-null with every viz "
        "off' is a statement about the vizzes, not an arms equality."),
    'pom_shell_side_entry': ('KEEP-TUNABLE',
        "the supersede/no-longer language names an earlier VARIANT inside the "
        "same S1d thread, not this flag; active A/B in "
        "docs/S1D_CLOSED_SHELL_PLAN.md."),
    'pom_prism_march': ('KEEP-TUNABLE',
        "same: an active S1d-5 research A/B (docs/S1D_CLOSED_SHELL_PLAN.md), "
        "default 0 and behind --pom_shell + --pom_prism."),
    'greets_displace_plane_normal': ('KEEP-TUNABLE',
        "active round-3 displacement research A/B (doorway-jamb ride "
        "direction); the supersede language is about round 2's de-slide."),
    'greets_displace_free_edge': ('DECIDE-LOOK',
        "the ledger refutation 10994f6ef014 is scoped to the VERTEX-COINCIDENCE "
        "test inside the flag ('the veto now probes a face soup within 0.05 u'), "
        "i.e. a superseded sub-method, NOT the flag; the owner's own 2026-08-12 "
        "note ('makes most of the sites better, but ... adds a bulge') is an "
        "open look call."),
}

# Sub-class of DELETE-LANDED-AB, decided by reading the row:
#   A = the two arms are byte-equal, so the OFF arm is provably dead weight.
#   B = a LANDED FIX: ON is the shipped behaviour, OFF is the LEGACY path kept
#       as a revert. Deleting B removes a revert hatch, so B is a separate call.
LANDED_SUBCLASS = {
    # A -- arms byte-equal
    'cone_fine_tiles': 'A', 'deferred_tile_sphere_cull': 'A',
    'vertex_light_parallel': 'A', 'tile_bbox_cull': 'A',
    'xpar_strip_extent': 'A', 'xpar_peel_early_out': 'A',
    'xfrm_soa_inline': 'A', 'vol_cone_lane_vec': 'A',
    # B -- landed fix, OFF is the legacy look
    'env_cube': 'B', 'mip_fix': 'B', 'env_bake_linear': 'B',
    'sh_bake_linear': 'B', 'metal_spec_f0': 'B',
    'env_metal_tint_linear': 'B', 'shadow_noncaster_depth': 'B',
    'env_bake_sh_first': 'B', 'env_bake_include_animated': 'B',
    'deferred_checker_env_full': 'B', 'greets_displace_seam_union': 'B',
    'greets_shatter_screen_mat_only': 'B', 'mirror_flare_bbox': 'B',
    'env_dyn_static_exclude': 'B', 'greets_displace_groove_shade': 'B',
}

def classify(f):
    """Return (bucket, [evidence strings]). First rule that matches wins;
    MANUAL_OVERRIDES wins over all of them."""
    ev = []
    name = f['name']
    if name in MANUAL_OVERRIDES:
        b, why = MANUAL_OVERRIDES[name]
        return b, ['HAND-ADJUDICATED: ' + why]

    # R1 ─ zero code references anywhere outside the .def
    if f['refs_code'] == 0:
        return 'DELETE-UNWIRED', ['0 code refs in DEMO/ FDS/ tools/ '
                                  '(accessor, *Id::, name-string, argv-string)']

    # R2 ─ refuted and parked at its no-op default
    ref = gw_refutes(f)
    if noop_default(f) and (re.search(r'\bREFUTED\b', f['help']) or ref):
        if re.search(r'\bREFUTED\b', f['help']):
            ev.append('help text says REFUTED')
        for g in ref:
            ev.append(f"ledger {g['id']} ({g['subject']})")
        ev.append('default is the no-op value')
        return 'DELETE-REFUTED', ev

    # R3 ─ landed and proven: default ON for >= 2 weeks, no later toggle
    osince = on_since(f)
    if (f['type'] == 'BOOL' and f['default'].strip() == '1' and osince
            and osince[0] <= LANDED_CUTOFF):
        pos = RE_BYTE_POS.search(f['help'])
        neg = RE_BYTE_NEG.search(f['help'])
        looky = RE_LOOK_PENDING.search(f['help'])
        if pos and not neg and not looky:
            ev.append(f'default ON since {osince[0]} ({osince[1]})')
            ev.append('help asserts the arms are byte/bit-equal: '
                      + f['help'][max(0, pos.start() - 60):pos.end() + 120].replace('\n', ' '))
            return 'DELETE-LANDED-AB', ev
        if looky and not neg:
            ev.append(f'default flipped ON {osince[0]} ({osince[1]}) — landed LOOK fix')
            ev.append('help still says the flag is parked OFF: STALE TEXT')
            return 'DELETE-LANDED-AB', ev

    # R4 ─ default-OFF look flags waiting on the owner's eye
    if noop_default(f) and (RE_LOOK_PENDING.search(f['help']) or gw_open_menu(f)):
        m = RE_LOOK_PENDING.search(f['help'])
        if m:
            ev.append(f['help'][max(0, m.start() - 200):m.end() + 160].replace('\n', ' '))
        for g in gw_open_menu(f):
            ev.append(f"ledger {g['id']} {g['subject']}: {g['claim'][:200]}")
        return 'DECIDE-LOOK', ev

    # R5 ─ instruments
    if RE_INSTRUMENT.search(f['help']) or RE_INSTR_NAME.search(name):
        scripts = [x for x in f['cli_files'] + f['env_files']
                   if x.startswith('tools/')]
        ev.append('instrument tag / name')
        if scripts:
            ev.append('used by ' + ', '.join(sorted(set(scripts))))
        return 'KEEP-DEBUG', ev

    # R6/R7 ─ contradictions go to UNSURE, everything else is a live tunable
    if RE_SUPERSEDED.search(f['help']):
        return 'UNSURE', ['help says superseded / no longer used, but the flag is '
                          'still read: needs a human read of the row']
    if (f['type'] == 'BOOL' and f['default'].strip() == '1' and osince
            and osince[0] > LANDED_CUTOFF and RE_BYTE_POS.search(f['help'])):
        age = osince[0]
        return 'UNSURE', [f'byte-equal A/B hatch but default ON only since {age} '
                          f'(< 2 weeks at {AUDIT_DATE}) — hold']
    return 'KEEP-TUNABLE', ['live tunable / scene control, %d code refs' % f['refs_code']]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=None)
    ap.add_argument('--md', default=None)
    args = ap.parse_args()
    flags, order = parse_def()
    print(f'[flag_audit] parsed {len(flags)} flags from {DEF}', file=sys.stderr)
    refs = scan_refs(flags)
    hist = git_history(flags)
    gwh  = groundwork(flags)
    print(f'[flag_audit] refs scanned; git history {len(hist)}; ledger hits {len(gwh)}',
          file=sys.stderr)

    rows = []
    for n in order:
        f = flags[n]; r = refs[n]; h = hist.get(n, []); g = gwh.get(n, [])
        proof = [c for c in h if PROOF_RE.search(c['msg'])]
        row = dict(f,
            refs_code=len(r['code']), refs_comment=len(r['comment']),
            refs_inloop=len([c for c in r['code'] if c['loop'] > 0 and c['kind'] != 'str']),
            refs_inloop_hot=len([c for c in r['code']
                                 if c['loop'] > 0 and c['hot'] and c['kind'] != 'str']),
            inloop_sites=[f"{x['file']}:{x['line']}(L{x['loop']})"
                          for x in r['code'] if x['loop'] > 0 and x['kind'] != 'str'][:8],
            code_sites=[f"{x['file']}:{x['line']}[{x['kind']}]" for x in r['code']][:16],
            env_files=sorted(set(r['env']))[:10],
            cli_files=sorted(set(r['cli']))[:16],
            first=h[-1]['date'] if h else None, first_sha=h[-1]['sha'] if h else None,
            last=h[0]['date'] if h else None, last_sha=h[0]['sha'] if h else None,
            last_msg=h[0]['msg'].split('\n')[0][:180] if h else None,
            n_commits=len(h),
            default_history=[(c['date'], c['sha'], c['added_default']) for c in h],
            proof_commits=[(c['sha'], c['date'], c['msg'].split('\n')[0][:150])
                           for c in proof][:4],
            gw=g)
        b, why = classify(row)
        row['bucket'] = b; row['why'] = why
        row['on_since'] = on_since(row)
        row['subclass'] = LANDED_SUBCLASS.get(n, '') if b == 'DELETE-LANDED-AB' else ''
        if b == 'DELETE-LANDED-AB' and not row['subclass']:
            row['bucket'] = 'UNSURE'
            row['why'] = ['matched the landed-AB rule but is UNCLASSIFIED in '
                          'LANDED_SUBCLASS -- a human has not read the row'] + why
        rows.append(row)

    out = args.json or os.path.join(ROOT, 'build', 'flag_audit.json')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w') as fh:
        json.dump(rows, fh, indent=1)
    from collections import Counter
    c = Counter(r['bucket'] for r in rows)
    for k in sorted(c):
        b = sum(r['help_len'] for r in rows if r['bucket'] == k)
        il = sum(r['refs_inloop'] for r in rows if r['bucket'] == k)
        print(f'  {k:20s} {c[k]:4d} flags  {b:7d} B help  {il:4d} in-loop reads',
              file=sys.stderr)
    print(f'[flag_audit] wrote {out} ({len(rows)} rows)', file=sys.stderr)

    if args.md:
        emit_md(rows, args.md)
        print(f'[flag_audit] wrote {args.md}', file=sys.stderr)


BUCKET_ORDER = ['DELETE-UNWIRED', 'DELETE-REFUTED', 'DELETE-LANDED-AB',
                'DECIDE-LOOK', 'UNSURE', 'KEEP-DEBUG', 'KEEP-TUNABLE']

def emit_md(rows, path):
    from collections import Counter
    L = []
    L.append('| bucket | flags | help-text bytes | in-loop reads | flags with an in-loop read |')
    L.append('|---|--:|--:|--:|--:|')
    for b in BUCKET_ORDER:
        sel = [r for r in rows if r['bucket'] == b]
        if not sel: continue
        L.append('| %s | %d | %d | %d | %d |' % (
            b, len(sel), sum(r['help_len'] for r in sel),
            sum(r['refs_inloop'] for r in sel),
            sum(1 for r in sel if r['refs_inloop'])))
    L.append('| **total** | **%d** | **%d** | **%d** | **%d** |' % (
        len(rows), sum(r['help_len'] for r in rows),
        sum(r['refs_inloop'] for r in rows),
        sum(1 for r in rows if r['refs_inloop'])))
    L.append('')
    L.append('| flag | type | default | bucket | refs | in-loop | last touch | evidence |')
    L.append('|---|---|---|---|--:|--:|---|---|')
    for b in BUCKET_ORDER:
        for r in rows:
            if r['bucket'] != b: continue
            il = ('%d %s' % (r['refs_inloop'], ' '.join(r['inloop_sites'][:2]))
                  if r['refs_inloop'] else '—')
            ev = ' · '.join(r['why'])[:400].replace('|', '\\|').replace('\n', ' ')
            bk = r['bucket'] + ('-' + r['subclass'] if r.get('subclass') else '')
            L.append('| `%s` | %s | `%s` | %s | %d | %s | %s %s | %s |' % (
                r['name'], r['type'], r['default'], bk, r['refs_code'], il,
                r['last'] or '—', r['last_sha'] or '', ev))
    open(path, 'w').write('\n'.join(L) + '\n')

if __name__ == '__main__':
    main()
