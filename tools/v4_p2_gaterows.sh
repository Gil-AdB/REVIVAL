#!/usr/bin/env bash
# How many rows of the phase-2 census gate FAIL, at the flag defaults.
# Prints "P2_GATE_FAILING_ROWS=<n>" plus the table on stderr.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/v4_p2_gaterows"
rm -rf "$OUT"; mkdir -p "$OUT"
"$ROOT/tools/v4_p2_pose.sh" "$OUT" 5965 \
  "22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \
  --greets_displace_v4 --v4_census >/dev/null
rm -f "$OUT"/*.ppm
python3 "$ROOT/tools/v4_census.py" --p2gate "$OUT/log.txt" > "$OUT/gate.txt"
cat "$OUT/gate.txt" >&2
echo "P2_GATE_FAILING_ROWS=$(grep -c '^  \[FAIL\]' "$OUT/gate.txt")"
