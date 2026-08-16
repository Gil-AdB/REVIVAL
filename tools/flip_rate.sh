#!/usr/bin/env bash
# flip_rate.sh — repeatable render-nondeterminism rate harness.
#
# Runs a headless snapshot recipe N times SEQUENTIALLY, hashes the output
# frame(s) of each run, and reports the distinct-hash histogram, the flip
# rate against the modal ("majority") hash, and a Wilson 95% CI on that rate.
#
# Why it exists: the greets scene has a long-standing ~1-in-12 run-to-run
# hash flip. Rate claims about it need the binomial, not a 2-run A/B — see
# docs/SESSION_STATE.md "Traps" and memory `measurement-tool-traps`. This
# script is the one instrument everything else hangs off, so keep it in-repo.
#
# Usage:
#   tools/flip_rate.sh -n 48 -l baseline                       # greets pin recipe
#   tools/flip_rate.sh -n 48 -l no_mirror -- --no-greets_mirror  # extra flags
#   tools/flip_rate.sh -n 24 -s fountain -l fnt                # another scene
#
# Options:
#   -n N        number of runs (default 24)
#   -l LABEL    label for the result dir (default "run")
#   -s SCENE    scene preset: greets | city | fountain (default greets)
#   -o DIR      results root (default $TMPDIR/flip_rate)
#   -b BINARY   DEMO binary NAME inside Runtime/ (default DEMO)
#   -k          keep per-run stderr logs (default: discard — logs perturb timing)
#   -j          load guard: wait for 1-min load avg below this before each run
#               (default 0 = no guard). The box is often shared with other agents.
#   --          everything after is appended to the DEMO command line
#
# Output: $DIR/$LABEL/hashes.tsv  (run<TAB>md5) plus a printed summary.
#
# NOTE: the binary is invoked from Runtime/ so ChdirToAssetRoot lands on the
# main tree's Runtime content (see memory: worktree-binary asset-root trap).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/Runtime"

N=24
LABEL="run"
SCENE="greets"
OUTROOT="${TMPDIR:-/tmp}/flip_rate"
BIN="DEMO"
KEEPLOGS=0
LOADGUARD=0
EXTRA=()

while [ $# -gt 0 ]; do
  case "$1" in
    -n) N="$2"; shift 2;;
    -l) LABEL="$2"; shift 2;;
    -s) SCENE="$2"; shift 2;;
    -o) OUTROOT="$2"; shift 2;;
    -b) BIN="$2"; shift 2;;
    -k) KEEPLOGS=1; shift;;
    -j) LOADGUARD="$2"; shift 2;;
    --) shift; EXTRA=("$@"); break;;
    *) echo "flip_rate.sh: unknown arg '$1'" >&2; exit 2;;
  esac
done

# ---- scene presets: the committed gate recipes (docs/SESSION_STATE.md) ----
# `--profiler=0` is on EVERY arm on purpose. It used to be on greets and
# fountain but not city, and that asymmetry cost three rounds (2026-08-16f):
# rev.cfg ships `ProfilerEnable 1`, each narrative tick paints the overlay into
# VPage, and only RunChaseSnapshot silenced it — so the city recipe below and
# the greets/fountain ones above were measuring two different things, and the
# city pins recorded without the flag carried 3 718 px of HUD text. The
# silencer now covers city/fountain/greets too (DEMO/Snapshot.cpp), which makes
# the flag INERT rather than load-bearing; it stays written down so the recipe
# cannot silently regress on an older binary.
declare -a FLAGS
case "$SCENE" in
  greets)
    SNAP="greets@t=1588"
    FLAGS=(--deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4
           --profiler=0 --no-env_refl)
    export FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551"
    ;;
  city)
    SNAP="city@t=1961"
    FLAGS=(--deferred --profiler=0)
    export FDS_CITY_ENV_PIXEL=1
    ;;
  fountain)
    SNAP="fountain@t=2500"
    FLAGS=(--deferred --hdr --glass-refract=1 --glass-test --profiler=0)
    ;;
  *) echo "flip_rate.sh: unknown scene '$SCENE'" >&2; exit 2;;
esac

export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy

DIR="$OUTROOT/$LABEL"
rm -rf "$DIR"; mkdir -p "$DIR"
TSV="$DIR/hashes.tsv"
: > "$TSV"

{
  echo "# scene=$SCENE snap=$SNAP bin=$BIN n=$N"
  echo "# flags: ${FLAGS[*]} ${EXTRA[*]:-}"
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# host load at start: $(uptime)"
} > "$DIR/recipe.txt"

cd "$RUN" || { echo "no Runtime/ at $RUN" >&2; exit 2; }
[ -x "./$BIN" ] || { echo "no executable Runtime/$BIN" >&2; exit 2; }

# 600 s watchdog per run, portable (no coreutils `timeout` on stock macOS).
run_once() { # $1 = out dir, $2 = log path
  local od="$1" lg="$2"
  ./"$BIN" --snapshot="$SNAP" --out="$od" "${FLAGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"} \
      > "$lg" 2>&1 &
  local pid=$!
  local waited=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    waited=$((waited+1))
    if [ "$waited" -ge 600 ]; then
      echo "WATCHDOG: killing run after ${waited}s" >> "$lg"
      kill -9 "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 124
    fi
  done
  wait "$pid"
  return $?
}

load_wait() {
  [ "$LOADGUARD" = "0" ] && return 0
  local tries=0
  while [ "$tries" -lt 120 ]; do
    local l
    l=$(uptime | sed 's/.*load averages*: *//' | awk '{print $1}' | tr -d ,)
    awk -v a="$l" -v b="$LOADGUARD" 'BEGIN{exit !(a<b)}' && return 0
    sleep 5; tries=$((tries+1))
  done
  return 0
}

echo "flip_rate: scene=$SCENE label=$LABEL n=$N bin=$BIN extra='${EXTRA[*]:-}'"
for i in $(seq 1 "$N"); do
  load_wait
  od="$DIR/f"; rm -rf "$od"; mkdir -p "$od"
  lg="$DIR/run$i.log"; [ "$KEEPLOGS" = "0" ] && lg="/dev/null"
  run_once "$od" "$lg"
  rc=$?
  if [ $rc -ne 0 ]; then
    h="ERR_rc$rc"
  else
    h=$(ls "$od"/*.ppm 2>/dev/null | sort | xargs cat 2>/dev/null | md5 -q)
    [ -z "$h" ] && h="ERR_noppm"
  fi
  printf '%d\t%s\n' "$i" "$h" >> "$TSV"
  printf '.'
  [ $((i % 24)) -eq 0 ] && printf ' %d\n' "$i"
done
printf '\n'
rm -rf "$DIR/f"

python3 - "$TSV" "$LABEL" <<'PY'
import sys, collections, math
tsv, label = sys.argv[1], sys.argv[2]
rows = [l.split('\t') for l in open(tsv).read().splitlines() if l.strip()]
hs = [r[1] for r in rows]
n = len(hs)
c = collections.Counter(hs)
modal, k_modal = c.most_common(1)[0]
k = n - k_modal                     # flips = runs differing from the mode
p = k / n if n else 0.0
# Wilson 95% CI
z = 1.959963985
if n:
    d = 1 + z*z/n
    ctr = (p + z*z/(2*n)) / d
    hw = z*math.sqrt(p*(1-p)/n + z*z/(4*n*n)) / d
    lo, hi = max(0.0, ctr-hw), min(1.0, ctr+hw)
else:
    lo = hi = 0.0
print(f"=== {label}: n={n} distinct={len(c)} ===")
for h, cnt in c.most_common():
    tag = " <- modal" if h == modal else ""
    runs = [r[0] for r in rows if r[1] == h]
    shown = ",".join(runs[:12]) + ("..." if len(runs) > 12 else "")
    print(f"  {cnt:4d}  {h}{tag}   runs: {shown}")
print(f"  flip rate = {k}/{n} = {p:.4f}   Wilson95% [{lo:.4f}, {hi:.4f}]"
      + (f"   (~1 in {1/p:.1f})" if p > 0 else "   (zero flips)"))
if k == 0 and n:
    # one-sided 95% upper bound on a zero-event rate: 1-0.05**(1/n)
    ub = 1 - 0.05**(1.0/n)
    print(f"  zero flips in {n}: 95% upper bound on true rate = {ub:.4f} (~1 in {1/ub:.0f})")
PY
