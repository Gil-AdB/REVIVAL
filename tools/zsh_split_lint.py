#!/usr/bin/env python3
"""zsh word-splitting lint.

zsh does NOT word-split an unquoted parameter: in a `#!/bin/zsh` script,
    RES="--force_xres=1920 --force_yres=1080"
    "$BIN" --snapshot=... $RES
passes ONE argument, "--force_xres=1920 --force_yres=1080", to the binary.
bash would pass two. The fix is `${=RES}` (or an array). This has bitten the
tree repeatedly (2026-09-01: tools/ovec/gates.sh rendered every row at
1920x768 and failed 12/12 with no pixel change), so it is a lint now.

Rule: in every tracked script that runs under zsh (shebang `zsh`, or a `.zsh`
extension), any variable ASSIGNED a double-quoted string containing whitespace
must not be expanded bare (`$NAME` / `${NAME}` outside double quotes). Inside
double quotes a single argument is what the author asked for; `${=NAME}`,
`${(z)NAME}`, `${(s: :)NAME}` are the explicit split forms and pass.

    tools/zsh_split_lint.py            # all tracked scripts
    tools/zsh_split_lint.py FILE...    # just these
Exit 1 on any finding.
"""
import re
import subprocess
import sys

ASSIGN = re.compile(r'^\s*(?:export\s+|local\s+|typeset\s+)?([A-Za-z_][A-Za-z0-9_]*)="([^"]*\s[^"]*)"')
SPLIT_FORMS = ('${=', '${(z)', '${(s')


def is_zsh(path, text):
    if path.endswith('.zsh'):
        return True
    first = text.split('\n', 1)[0]
    return first.startswith('#!') and 'zsh' in first


def bare_uses(line, name):
    """Yield column offsets of $NAME / ${NAME} outside double quotes."""
    in_dq = False
    in_sq = False
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '\\' and not in_sq:
            i += 2
            continue
        if c == "'" and not in_dq:
            in_sq = not in_sq
        elif c == '"' and not in_sq:
            in_dq = not in_dq
        elif c == '#' and not in_dq and not in_sq and (i == 0 or line[i - 1].isspace()):
            return
        elif c == '$' and not in_dq and not in_sq:
            rest = line[i:]
            m = re.match(r'\$(?:\{' + name + r'\}|' + name + r'(?![A-Za-z0-9_]))', rest)
            if m:
                yield i
        i += 1


def lint_file(path):
    try:
        text = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return []
    if not is_zsh(path, text):
        return []
    lines = text.split('\n')
    spaced = {}
    for ln, line in enumerate(lines, 1):
        m = ASSIGN.match(line)
        if m:
            spaced[m.group(1)] = ln
    findings = []
    for name, aln in spaced.items():
        for ln, line in enumerate(lines, 1):
            if ln == aln or not line.strip() or line.lstrip().startswith('#'):
                continue
            for col in bare_uses(line, name):
                findings.append((path, ln, col + 1, name, aln, line.strip()))
    return findings


def main(argv):
    if argv:
        files = argv
    else:
        out = subprocess.run(['git', 'ls-files', '--', '*.sh', '*.zsh', 'tools/*', 'tools/**/*'],
                             capture_output=True, text=True, check=False).stdout.split('\n')
        files = sorted({f for f in out if f})
    findings = []
    for f in files:
        findings.extend(lint_file(f))
    for path, ln, col, name, aln, src in findings:
        print(f'{path}:{ln}:{col}: zsh: bare ${name} is ONE argument (assigned with spaces at line {aln}); '
              f'use ${{={name}}}   | {src}')
    if findings:
        print(f'zsh_split_lint: {len(findings)} finding(s)')
        return 1
    print('zsh_split_lint: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
