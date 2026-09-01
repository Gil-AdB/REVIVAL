#!/usr/bin/env bash
# tools/lint.sh — the standing structural checks for this tree.
#
#   tools/lint.sh              run everything, exit non-zero if anything fails
#   tools/lint.sh flags        only tools/flag_readers.py
#   tools/lint.sh semgrep      only the .semgrep/revival.yml rules
#   tools/lint.sh --staged     restrict the pattern rules to STAGED files
#   tools/lint.sh --json OUT   also write the semgrep findings as JSON to OUT
#
# There is no test suite here and no CI (CLAUDE.md says so outright), so these
# are the only checks that run without a human watching pixels. Both are cheap:
# flag_readers is a second, semgrep about ten.
#
# THE TRAP THIS SCRIPT EXISTS TO ABSORB — semgrep cannot see this repo's
# uppercase sources. It maps extensions to languages case-sensitively, so
# `REV.CPP`, `GREETS.CPP`, `CITY.CPP` and `FDS_DECS.H` are "unknown extensions"
# and get dropped at TARGET SELECTION, before any rule runs. Measured
# 2026-08-30: `semgrep --config .semgrep DEMO FDS` scanned 667 files — 589 .h,
# 77 .cpp, 1 .inl, ZERO .CPP — i.e. every scene file in DEMO/ was silently
# skipped, with no warning and exit 0. `--scan-unknown-extensions` alone does
# not fix it either (still 667). The fix is BOTH: that flag AND naming the
# files as explicit targets, which is what enumerate_targets does below.
set -uo pipefail

ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
cd "$ROOT" || exit 2

VENV="$ROOT/tools/.venv-semgrep"
SEMGREP="$VENV/bin/semgrep"
RULES="$ROOT/.semgrep/revival.yml"

want_flags=1; want_semgrep=1; json_out=""; staged=0
while [ $# -gt 0 ]; do
	case "$1" in
		flags)   want_semgrep=0 ;;
		semgrep) want_flags=0 ;;
		--staged) staged=1 ;;
		--json)  json_out="${2:?--json needs a path}"; shift ;;
		-h|--help) sed -n '2,9p' "$0"; exit 0 ;;
		*) echo "lint.sh: unknown argument '$1'" >&2; exit 2 ;;
	esac
	shift
done

rc=0

# ── 1. every FDS_FLAG_* in FeatureFlags.def has a reader ────────────────────
if [ "$want_flags" = 1 ]; then
	echo "== flag_readers =="
	python3 "$ROOT/tools/flag_readers.py" || rc=1
fi

# ── 1b. zsh word-splitting: a bare $VAR holding "a b" is ONE argument ───────
# (tools/zsh_split_lint.py; --staged narrows it to the staged scripts).
if [ "$want_flags" = 1 ]; then
	echo "== zsh_split_lint =="
	if [ "$staged" = 1 ]; then
		zsh_targets="$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(sh|zsh)$|^tools/' || true)"
		if [ -n "$zsh_targets" ]; then
			# shellcheck disable=SC2086
			python3 "$ROOT/tools/zsh_split_lint.py" $zsh_targets || rc=1
		else
			echo "-- no staged scripts"
		fi
	else
		python3 "$ROOT/tools/zsh_split_lint.py" || rc=1
	fi
fi

# ── 2. the pattern rules ────────────────────────────────────────────────────
enumerate_targets() {
	# Tracked sources only (git ls-files), both case conventions, minus the
	# vendored trees and the pristine 1998 reference. Vendored code is not ours
	# to fix and Original/ is read-only by policy (CLAUDE.md).
	#
	# --staged narrows this to what is about to be committed, which is what the
	# pre-commit hook wants: the tree carries pre-existing findings (one ERROR,
	# ~100 WARNINGs), so a hook that scanned everything would refuse every
	# commit and be uninstalled within the hour. Scanning the staged set means
	# the hook fires only on a file YOU touched.
	if [ "$staged" = 1 ]; then
		git diff --cached --name-only --diff-filter=ACMR -z
	else
		git ls-files -z -- \
			'DEMO/*' 'FDS/*' 'tests/*' 'GpuBench/*' 'tools/*.sh' 'scratchpad/*.sh'
	fi \
	| tr '\0' '\n' \
	| grep -E '\.(cpp|CPP|cc|h|H|hpp|inl|sh)$' \
	| grep -v -E '^FDS/(simd|simde)/' \
	| grep -v -E '^Original/'
}

if [ "$want_semgrep" = 1 ]; then
	echo "== semgrep =="
	if [ ! -x "$SEMGREP" ]; then
		echo "lint.sh: no semgrep at $SEMGREP — bootstrapping (~150 MB of wheels)."
		echo "lint.sh: check free disk first if that matters:  df -h /"
		python3 -m venv "$VENV" && "$VENV/bin/pip" install --quiet semgrep || {
			echo "lint.sh: semgrep bootstrap FAILED; skipping the pattern rules" >&2
			exit 2
		}
	fi

	targets="$(mktemp)"; raw="$(mktemp)"
	trap 'rm -f "$targets" "$raw"' EXIT
	enumerate_targets > "$targets"
	n=$(wc -l < "$targets" | tr -d ' ')
	if [ "$n" = 0 ]; then
		echo "-- no source targets (staged set is empty of them); nothing to scan"
		exit $rc
	fi
	nupper=$(grep -c -E '\.(CPP|H)$' "$targets")
	echo "-- $n targets ($nupper of them uppercase-extension, which a bare semgrep would skip)"

	# ONE semgrep run, JSON only; the report below is rendered from it. Two
	# runs (text + json) would double an 8 s scan for nothing, and the text
	# renderer cannot show the thing that matters most here — which files the
	# C++ parser REFUSED, and therefore which findings could not exist.
	#
	# NUL-separated: BSD xargs has no -a and no -d, and a bare `xargs` would
	# split on the quotes in a path.
	tr '\n' '\0' < "$targets" \
	  | xargs -0 "$SEMGREP" --config "$RULES" --metrics=off \
	          --scan-unknown-extensions --no-git-ignore --disable-version-check \
	          --json --output "$raw" -- >/dev/null 2>&1
	if [ ! -s "$raw" ]; then
		echo "lint.sh: semgrep produced no output — treating as failure" >&2
		exit 2
	fi
	[ -n "$json_out" ] && cp "$raw" "$json_out" && echo "-- findings JSON: $json_out"

	python3 - "$raw" <<'PY' || rc=1
import json, sys, collections
d = json.load(open(sys.argv[1]))
res = d.get("results", [])

# ── the blind spot, reported FIRST because it bounds everything below ───────
whole = []
for e in d.get("errors", []):
    t = e.get("type"); t = t if isinstance(t, str) else t[0]
    if t in ("Syntax error", "Other syntax error"):
        whole.append(e.get("path"))
if whole:
    print(f"-- UNPARSED: semgrep's C++ parser rejected {len(whole)} file(s) OUTRIGHT.")
    print("   No rule ran on them. A clean report is NOT a clean file here:")
    for p in sorted(set(whole)):
        print(f"     {p}")

by_rule = collections.defaultdict(list)
for r in res:
    by_rule[r["check_id"].split(".")[-1]].append(r)

errors = 0
for rule in sorted(by_rule):
    rows = by_rule[rule]
    sev = rows[0]["extra"].get("severity", "WARNING")
    print(f"\n-- {rule}  [{sev}]  {len(rows)} finding(s)")
    if sev == "ERROR":
        errors += len(rows)
        for r in rows:
            print(f"     {r['path']}:{r['start']['line']}")
    else:
        # WARNING rules are a census: per-file counts, not 100 identical lines.
        c = collections.Counter(r["path"] for r in rows)
        for p, k in c.most_common():
            print(f"     {k:3d}  {p}")
for rule in ("revival-raw-getenv", "revival-shell-bulk-git-mutation",
             "revival-featureflags-at-namespace-scope"):
    if rule not in by_rule:
        print(f"\n-- {rule}: 0 findings")

print(f"\n-- total {len(res)} finding(s); {errors} at ERROR severity")
# Only ERROR severity gates. The raw-getenv census is a WARNING on purpose:
# it is ~100 pre-existing call sites, and a check that can never pass is a
# check everyone learns to skip.
sys.exit(1 if errors else 0)
PY
fi

exit $rc
