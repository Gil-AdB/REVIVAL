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
awk '/holes=/{n=$0; sub(/.*holes=/,"",n); sub(/ .*/,"",n); s+=n; c++; if (n+0>0) p++}
     END{printf "v4_tear_battery: poses=%d poses_with_holes=%d TOTAL_HOLES=%d\n", c, p+0, s+0}' "$OUT/battery.txt"
