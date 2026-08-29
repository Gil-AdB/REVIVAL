#!/bin/zsh
# Render the reference relief renderer's pose battery.
#   tools/refrender_battery.sh <outdir>
# Arms per pose: bare | tessellated default | reference. Extra arms at cam A and
# H6194 only (mitre-limit, crease, shared-edge, partitioned membership).
# Never opens a window: SDL dummy drivers on every launch.
set -e
OUT=${1:-/tmp/refrender}
RT=$(cd "$(dirname "$0")/../Runtime" && pwd)
JUDGE=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
mkdir -p "$OUT"

# name t cam
POSES=(
"camA|5965|22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499"
"camB|5965|20.7104416,3.05759621,-59.807045,-0.863270998,-0.251949817,0.437360346"
"H6017|6017|19.3172779,5.18087387,-58.6863327,-0.946125329,-0.108705439,0.305008113"
"H6194|6194|22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849"
"doorway5963|5963|18.5616322,3.21013689,-58.8002586,-0.886640549,-0.0750753954,0.456324756"
"doorway5928|5928|18.5616322,3.21013689,-58.8002586,-0.886640549,-0.0750753954,0.456324756"
"corner6097|6097|18.4499683,5.16043377,-57.6482239,-0.824408829,-0.544822097,-0.153357133"
"curved2845|2845|-7.38721609,2.72471762,-50.8239441,0.817980111,-0.113630958,0.563911617"
"corridor5534|5534|8.1065979,3.19413304,-39.0308495,-0.0432227664,-0.188236833,-0.981172085"
)

run() {   # run <dir> <t> <cam> <extra flags...>
  local d=$1 t=$2 cam=$3; shift 3
  mkdir -p "$d"
  ( cd "$RT" && FDS_GREETS_CAM="$cam" FDS_REFRENDER_DUMP="$d/ref.bin" \
      FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 FDS_SNAPSHOT_NRMDUMP=1 \
      ./DEMO ${JUDGE[@]} "$@" --snapshot=greets@t=$t --out="$d" ) > "$d/log.txt" 2>&1
}

for row in $POSES; do
  name=${row%%|*}; rest=${row#*|}; t=${rest%%|*}; cam=${rest#*|}
  echo "== $name t=$t"
  run "$OUT/$name/bare"  $t "$cam"
  run "$OUT/$name/tess"  $t "$cam" --greets-displace
  run "$OUT/$name/ref"   $t "$cam" --greets_displace_ref --greets_displace_ref_stats=1
  run "$OUT/$name/refse" $t "$cam" --greets_displace_ref --greets_displace_ref_shared_edge
done

for name t cam in camA 5965 22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499 \
                  H6194 6194 22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849; do
  echo "== variants $name"
  run "$OUT/$name/ref_part1"   $t "$cam" --greets_displace_ref --greets_displace_ref_partition=1
  run "$OUT/$name/ref_mitre2"  $t "$cam" --greets_displace_ref --greets_displace_ref_mitre=2
  run "$OUT/$name/ref_mitre8"  $t "$cam" --greets_displace_ref --greets_displace_ref_mitre=8
  run "$OUT/$name/ref_crease15" $t "$cam" --greets_displace_ref --greets_displace_ref_crease=15
  run "$OUT/$name/ref_crease60" $t "$cam" --greets_displace_ref --greets_displace_ref_crease=60
done
echo "done -> $OUT"
