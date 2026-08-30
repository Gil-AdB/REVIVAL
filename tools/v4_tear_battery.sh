#!/usr/bin/env bash
# 54-pose HOLE battery for the v4 UNDISPLACED lattice (docs/tears_poses.txt).
# Each pose renders the control (--v4_flat) and the lattice arm and compares
# coverage; see tools/v4_tear_cover.py for why a control reference rather than
# the rev-dispfix ground-truth detector.
#   tools/v4_tear_battery.sh [outroot]        outroot MUST be absolute
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${TMPDIR:-/tmp}/v4_tear}"
ARM="${2:-lattice}"                       # lattice | old0 (the OLD bake at amp=0)
TSTFLAGS=(--greets_displace_v4)
[[ "$ARM" == "old0" ]] && TSTFLAGS=(--greets_displace_amp=0)
case "$OUT" in /*) ;; *) echo "outroot must be ABSOLUTE"; exit 2 ;; esac
rm -rf "$OUT"; mkdir -p "$OUT"
tot=0; bad=0; n=0
grep -v '^#' "$ROOT/docs/tears_poses.txt" | while read -r id t cam; do
  [[ -z "${id:-}" || -z "${t:-}" ]] && continue
  C="${cam:-}"; [[ "$C" == "-" ]] && C=""
  if [[ -z "$C" ]]; then
    "$ROOT/tools/v4_p2_pose.sh" "$OUT/a" "$t" "0,0,0,0,0,1" --greets_displace_v4 --v4_flat >/dev/null
    "$ROOT/tools/v4_p2_pose.sh" "$OUT/b" "$t" "0,0,0,0,0,1" "${TSTFLAGS[@]}" >/dev/null
  else
    "$ROOT/tools/v4_p2_pose.sh" "$OUT/a" "$t" "$C" --greets_displace_v4 --v4_flat >/dev/null
    "$ROOT/tools/v4_p2_pose.sh" "$OUT/b" "$t" "$C" "${TSTFLAGS[@]}" >/dev/null
  fi
  python3 "$ROOT/tools/v4_tear_cover.py" "$OUT/a" "$OUT/b" --label "$id"
  rm -rf "$OUT/a" "$OUT/b"
done | tee "$OUT/battery.txt"
echo "--- v4_tear_battery: $(grep -c 'holes=' "$OUT/battery.txt") poses, "\
"$(awk -F'holes=| ' '/holes=/{s+=$2} END{print s+0}' "$OUT/battery.txt") total holes, "\
"$(grep -c 'holes=[1-9]' "$OUT/battery.txt") poses with holes ---"
