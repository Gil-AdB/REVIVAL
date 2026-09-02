#!/usr/bin/env bash
# v4 PHASE-4 geometric gate: the arm's dz against the REFERENCE RELIEF RENDERER
# restricted to the CREASE BAND — the band around a real dihedral junction,
# which is the only place §2e's ring solve changes anything.
#
#   tools/v4_p4_crease.sh <outdir> [pose ...]
#
# Why this and not tools/v4_p3_ref.sh: that script masks to BLOCK INTERIORS
# (--flat-deg), the flat face of a block, and P4 moves no block interior at all.
# Measured at the tip, the P3 and P4 arms differ by 0.5% on that mask and by
# 2% on its p90 — not because the rings did nothing but because the mask
# cannot see them.  The crease band is the complementary question, and both
# come off the same reference render through the same faceId plane table.
#
# Four arms per pose, all headless, all at 1920x1080:
#   ref   the reference renderer (the definition; carries ref.bin)
#   p4    --greets_displace_v4                       (rings ON, the default)
#   p3    --greets_displace_v4 --no-v4_rings         (the isolating control)
#   bare  --greets_displace_v4 --v4_flat             (authored triangles)
# The reference is rendered ONCE per pose and scored against all three.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$1"; shift
POSES=("$@")
if [ "${#POSES[@]}" -eq 0 ]; then
  POSES=(
    "camA|5965|22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
    "H6194|6194|22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849"
    "corner6097|6097|18.4499683,5.16043377,-57.6482239,-0.824408829,-0.544822097,-0.153357133"
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
  run "$p/p4"   "$t" "$cam" --greets-displace --greets_displace_v4
  run "$p/p3"   "$t" "$cam" --greets-displace --greets_displace_v4 --no-v4_rings
  run "$p/bare" "$t" "$cam" --greets-displace --greets_displace_v4 --v4_flat
  # Scored at three band radii on purpose.  2 px is the crease itself, where P3
  # pinned the ring at 0 and P4 gives it the dominant owner's height; 6 px is
  # the default band; 24 px reaches a whole border segment in and is dominated
  # by the block interiors P4 does not touch.  A phase that moved the crease
  # and nothing else shows up as a gap between the 2 px and the 24 px column.
  for r in ${V4P4_BANDS:-2 6 24}; do
    for arm in p4 p3; do
      python3 "$ROOT/tools/refrender_diff.py" --tess "$p/$arm" --ref "$p/ref" \
          --bare "$p/bare" --crease-band "$r" --json \
          > "$p/score_${arm}_r$r.json" 2>"$p/score_${arm}_r$r.err" \
          || cat "$p/score_${arm}_r$r.err" >&2
    done
    python3 - "$name" "$r" "$p/score_p4_r$r.json" "$p/score_p3_r$r.json" <<'PY'
import json,sys
n,r,f4,f3=sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4]
try:
    d4=json.load(open(f4)); d3=json.load(open(f3))
except Exception as e:
    print("%s SCORE-FAILED %s"%(n,e)); raise SystemExit
g=lambda d,k: d.get(k,float('nan'))
print("%-12s band=%-3s crease_px=%7d | CREASE p50 p3=%.4f p4=%.4f (%+.1f%%)  "
      "p90 p3=%.4f p4=%.4f"
      %(n,r,g(d4,"crease_px"),
        g(d3,"dz_p50_crease"),g(d4,"dz_p50_crease"),
        100.0*(g(d4,"dz_p50_crease")/max(1e-9,g(d3,"dz_p50_crease"))-1.0),
        g(d3,"dz_p90_crease"),g(d4,"dz_p90_crease")))
PY
  done
  cp "$p/score_p4_r6.json" "$p/score_p4.json" 2>/dev/null || true
  cp "$p/score_p3_r6.json" "$p/score_p3.json" 2>/dev/null || true
  python3 - "$name" "$p/score_p4.json" "$p/score_p3.json" <<'PY'
import json,sys
n,f4,f3=sys.argv[1],sys.argv[2],sys.argv[3]
try:
    d4=json.load(open(f4)); d3=json.load(open(f3))
except Exception as e:
    print("%s SCORE-FAILED %s"%(n,e)); raise SystemExit
g=lambda d,k: d.get(k,float('nan'))
print("%-12s crease_px=%7d | CREASE p50 p3=%.4f p4=%.4f  p90 p3=%.4f p4=%.4f | "
      "FLAT p50 p3=%.4f p4=%.4f  p90 p3=%.4f p4=%.4f"
      %(n,g(d4,"crease_px"),
        g(d3,"dz_p50_crease"),g(d4,"dz_p50_crease"),
        g(d3,"dz_p90_crease"),g(d4,"dz_p90_crease"),
        g(d3,"dz_p50_flat"),g(d4,"dz_p50_flat"),
        g(d3,"dz_p90_flat"),g(d4,"dz_p90_flat")))
PY
  # The raw planes are ~1 GB per pose, so they go unless asked for.  Set
  # V4P4_KEEP=1 when the score itself needs interrogating (an outlier p90 has
  # to be located on the image, not narrated).
  [ "${V4P4_KEEP:-0}" = "1" ] || rm -f "$p"/*/ref.bin "$p"/*/*.z16 "$p"/*/*.u32 "$p"/*/*.ppm
done
