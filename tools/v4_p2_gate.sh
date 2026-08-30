#!/usr/bin/env bash
# v4 PHASE-2 GATE (docs/DISPLACEMENT_V4_DESIGN.md §6 P2).
#
#   tools/v4_p2_gate.sh              # cam A + the review poses, two-tier
#   tools/v4_p2_gate.sh --poses N    # first N review poses (default all)
#   tools/v4_p2_gate.sh --control old   # compare against the OLD bake at amp=0
#                                       # instead of --v4_flat
#
# TIER 1 is the design's invariant: the undisplaced lattice renders
# byte-identically to the same pipeline WITHOUT the lattice.  The control is
# `--greets_displace_v4 --v4_flat` (the authored 226 triangles through the very
# same bake, mesh rebuild, parent-plane stamping and downstream pipeline), NOT
# `--no-greets_displace` — the latter also swaps the POM input map and two
# companion flags, so it would not isolate the retessellation.
#
# TIER 2, when tier 1 fails: |z16 delta| <= 1 quantum everywhere, ZERO
# raster-vs-empty flips, ZERO material-ownership flips against a non-stone
# sheet, and the colour deltas enumerated per pose.  Never opens a window; the
# raw planes are deleted per pose.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/v4_p2_gate"
NPOSE=999
CONTROL="flat"
TEST="lattice"
while (( $# )); do
  case "$1" in
    --poses) NPOSE="$2"; shift 2 ;;
    --control) CONTROL="$2"; shift 2 ;;
    --test) TEST="$2"; shift 2 ;;
    *) shift ;;
  esac
done
CTLFLAGS=(--greets_displace_v4 --v4_flat)
[[ "$CONTROL" == "old" ]] && CTLFLAGS=(--greets_displace_amp=0)
# --test old0 replaces the v4 lattice with the OLD bake at amp=0: the same
# question asked of the SHIPPED bake, which is what says whether a tier-2 miss
# is a v4 property or a property of retessellating this scene at all.
TSTFLAGS=(--greets_displace_v4)
[[ "$TEST" == "old0" ]] && TSTFLAGS=(--greets_displace_amp=0)

rm -rf "$OUT"; mkdir -p "$OUT"
CAMA="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
{ echo "5965 $CAMA"
  grep -vE '^\s*#|^\s*$' "$ROOT/docs/greets_review_poses.txt" | awk '{print $1, $2}'
} | head -n "$NPOSE" > "$OUT/poses.txt"

tier1=0; tier2=0; fail=0; n=0
while read -r T CAM; do
  [[ -z "${T:-}" ]] && continue
  n=$((n+1))
  "$ROOT/tools/v4_p2_pose.sh" "$OUT/a" "$T" "$CAM" "${CTLFLAGS[@]}" >/dev/null
  "$ROOT/tools/v4_p2_pose.sh" "$OUT/b" "$T" "$CAM" "${TSTFLAGS[@]}" >/dev/null
  python3 "$ROOT/tools/v4_p2_planes.py" "$OUT/a" "$OUT/b" --label "t=$T"
  rc=$?
  case $rc in 0) tier1=$((tier1+1)) ;; 1) tier2=$((tier2+1)) ;; *) fail=$((fail+1)) ;; esac
  rm -rf "$OUT/a" "$OUT/b"
done < "$OUT/poses.txt"

echo "v4_p2_gate: control=$CONTROL test=$TEST poses=$n tier1=$tier1 tier2_pass=$tier2 tier2_fail=$fail"
(( fail == 0 && tier2 == 0 )) && { echo "v4_p2_gate: TIER1 PASS"; exit 0; }
(( fail == 0 )) && { echo "v4_p2_gate: TIER2 PASS"; exit 1; }
echo "v4_p2_gate: FAIL"
exit 2
