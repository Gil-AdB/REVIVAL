#!/usr/bin/env bash
# 54-pose control-referenced HOLE battery for an ARBITRARY v4 arm, resumable.
#
#   tools/v4_tear_arm.sh <tag> [extra DEMO flags...]
#
# Same semantics as tools/v4_tear_battery.sh (control = the SAME pipeline with
# --v4_flat, per the per-polygon-mip trap ba7c2e825b1f: the control may never be
# --no-greets_displace), but the arm is whatever flags follow the tag, and the
# accumulator /tmp/v4tear_<tag>.txt is APPEND-ONLY and skipped-if-present, so a
# battery interrupted mid-run resumes where it stopped instead of restarting.
# Prints the battery total on stdout at the end -- that line is the ledger's
# recheck value.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:?usage: v4_tear_arm.sh <tag> [flags...]}"; shift
ACC="${TMPDIR:-/tmp}/v4tear_$TAG.txt"
OUT="${TMPDIR:-/tmp}/v4tear_work_$TAG"
touch "$ACC"
grep -v '^#' "$ROOT/docs/tears_poses.txt" | while read -r id t cam; do
  [ -z "${id:-}" ] && continue
  grep -q "^$id:" "$ACC" && continue
  C="${cam:-}"; [ "$C" = "-" ] && C=""
  [ -z "$C" ] && C="0,0,0,0,0,1"
  rm -rf "$OUT"; mkdir -p "$OUT"
  "$ROOT/tools/v4_p2_pose.sh" "$OUT/a" "$t" "$C" --greets_displace_v4 --v4_flat >/dev/null
  "$ROOT/tools/v4_p2_pose.sh" "$OUT/b" "$t" "$C" --greets_displace_v4 "$@" >/dev/null
  python3 "$ROOT/tools/v4_tear_cover.py" "$OUT/a" "$OUT/b" --label "$id" >> "$ACC"
  rm -rf "$OUT/a" "$OUT/b"
done
awk -F'holes=' '{split($2,a," "); s+=a[1]; if (a[1]+0>0) p++}
  END{printf "v4_tear_arm: poses=%d poses_with_holes=%d TOTAL_HOLES=%d\n", NR, p+0, s+0}' "$ACC"
