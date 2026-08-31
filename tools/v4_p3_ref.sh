#!/usr/bin/env bash
# v4 PHASE-3 geometric gate: the arm's dz against the REFERENCE RELIEF RENDERER,
# restricted to BLOCK INTERIORS, with the flat wall through the same mask as the
# floor (design §6 P3: "dz p50 at block interiors <= 2x the bare floor").
#
#   tools/v4_p3_ref.sh <outdir> [pose ...]      poses default to the corpus four
#
# Three arms per pose, all headless, all at 1920x1080:
#   ref   the reference renderer (the definition; carries ref.bin)
#   tess  --greets_displace_v4                (the phase-3 bake)
#   bare  --greets_displace_v4 --v4_flat      (the SAME pipeline, authored
#         triangles, no lattice -- the control trap ba7c2e825b1f demands, and
#         the "bare floor" the phase's ratio is taken against)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$1"; shift
POSES=("$@")
if [ "${#POSES[@]}" -eq 0 ]; then
  POSES=(
    "camA|5965|22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
    "camB|5965|20.7104416,3.05759621,-59.807045,-0.863270998,-0.251949817,0.437360346"
    "H6017|6017|19.3172779,5.18087387,-58.6863327,-0.946125329,-0.108705439,0.305008113"
    "curved2845|2845|-7.38721609,2.72471762,-50.8239441,0.817980111,-0.113630958,0.563911617"
  )
fi
mkdir -p "$OUT"
run() { local d="$1" t="$2" cam="$3"; shift 3; mkdir -p "$d"
  ( cd "$ROOT/Runtime" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      FDS_GREETS_CAM="$cam" FDS_REFRENDER_DUMP="$d/ref.bin" \
      FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 FDS_SNAPSHOT_NRMDUMP=1 \
      ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
             --force_xres=1920 --force_yres=1080 \
             "$@" --snapshot=greets@t=$t --out="$d" ) >"$d/log.txt" 2>&1; }
for row in "${POSES[@]}"; do
  name="${row%%|*}"; rest="${row#*|}"; t="${rest%%|*}"; cam="${rest#*|}"
  p="$OUT/$name"; rm -rf "$p"; mkdir -p "$p"
  run "$p/ref"  "$t" "$cam" --greets_displace_ref --greets_displace_ref_stats=1
  run "$p/tess" "$t" "$cam" --greets-displace --greets_displace_v4 ${V4P3_ARM:-}
  run "$p/bare" "$t" "$cam" --greets-displace --greets_displace_v4 --v4_flat
  python3 "$ROOT/tools/refrender_diff.py" --tess "$p/tess" --ref "$p/ref" --bare "$p/bare" \
      --json > "$p/score.json" 2>"$p/score.err" || cat "$p/score.err" >&2
  python3 - "$name" "$p/score.json" <<'PY'
import json,sys
n,f=sys.argv[1],sys.argv[2]
try: d=json.load(open(f))
except Exception as e: print("%s SCORE-FAILED %s"%(n,e)); raise SystemExit
print("%-12s flat_px=%7d dz_p50_flat=%.4f bare_p50_flat=%.4f ratio=%.2f %s | "
      "dz_p50_all=%.4f bare_p50_all=%.4f dz_p90_flat=%.4f"
      %(n,d.get("flat_px",0),d.get("dz_p50_flat",float('nan')),
        d.get("bare_dz_p50_flat",float('nan')),d.get("p3_gate_ratio",float('nan')),
        "PASS" if d.get("p3_gate_pass") else "FAIL",
        d.get("dz_p50",float('nan')),d.get("bare_dz_p50",float('nan')),
        d.get("dz_p90_flat",float('nan'))))
PY
  rm -f "$p"/*/ref.bin "$p"/*/*.z16 "$p"/*/*.u32 "$p"/*/*.ppm
done
