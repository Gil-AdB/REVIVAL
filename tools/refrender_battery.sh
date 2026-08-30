#!/bin/zsh
# Render the reference relief renderer's pose battery.
#   tools/refrender_battery.sh [options] <outdir>
#
# Arms per pose: bare | tessellated default | reference | reference+shared-edge.
# Extra arms at cam A and H6194 only (mitre-limit, crease, partitioned membership).
# Never opens a window: SDL dummy drivers on every launch.
#
# DISK.  The raw planes are 24 B/px for the reference dump plus 16 B/px of
# snapshot planes plus a 6 MB PPM, i.e. ~70 MB per reference arm at 1920x1080;
# the full battery in --keep-raw is ~2.5 GB and a previous run filled the disk
# mid-battery.  So the DEFAULT is LEAN: every pose is scored the moment it is
# rendered and its raw planes are deleted immediately, leaving only PNGs and the
# diff JSON.  `df -h /` is printed before the first pose and after every pose,
# the run ABORTS below --min-free GiB, and it aborts again if the output tree
# itself passes --max-out MiB.  Pass --keep-raw only when a later step needs the
# planes, and then only for a pose or two (--poses).
set -e

OUT=""
LEAN=1
MINFREE_GB=5
MAXOUT_MB=2048
POSES_WANT=""
ARMS_WANT="bare,tess,ref,refse"
VARIANTS=1
EXTRA=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep-raw)   LEAN=0; shift ;;
    --lean)       LEAN=1; shift ;;
    --poses)      POSES_WANT="$2"; shift 2 ;;
    --arms)       ARMS_WANT="$2"; shift 2 ;;
    --no-variants) VARIANTS=0; shift ;;
    --min-free)   MINFREE_GB="$2"; shift 2 ;;
    --max-out)    MAXOUT_MB="$2"; shift 2 ;;
    --extra)      EXTRA="$2"; shift 2 ;;
    -h|--help)    sed -n '2,25p' "$0"; exit 0 ;;
    -*)           echo "unknown option: $1" >&2; exit 2 ;;
    *)            OUT="$1"; shift ;;
  esac
done
: ${OUT:=/tmp/refrender}

HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../Runtime" && pwd)
DIFF="$HERE/refrender_diff.py"
JUDGE=(--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
mkdir -p "$OUT"

# ── disk guard ─────────────────────────────────────────────────────────────
free_gb() { df -g / | awk 'NR==2{print $4}' }
out_mb()  { du -sm "$OUT" 2>/dev/null | awk '{print $1}' }
guard() {
  local where="$1"
  df -h / | tail -1
  local f=$(free_gb)
  if (( f < MINFREE_GB )); then
    echo "ABORT ($where): only ${f} GiB free on /, floor is ${MINFREE_GB} GiB" >&2; exit 3
  fi
  local o=$(out_mb)
  if [[ -n "$o" ]] && (( o > MAXOUT_MB )); then
    echo "ABORT ($where): $OUT is ${o} MiB, cap is ${MAXOUT_MB} MiB" >&2; exit 3
  fi
}
echo "== disk before the battery (floor ${MINFREE_GB} GiB, out cap ${MAXOUT_MB} MiB)"
guard "start"

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

want() { [[ ",$1," == *",$2,"* ]] }

run() {   # run <dir> <t> <cam> <extra flags...>
  local d=$1 t=$2 cam=$3; shift 3
  mkdir -p "$d"
  ( cd "$RT" && FDS_GREETS_CAM="$cam" FDS_REFRENDER_DUMP="$d/ref.bin" \
      FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 FDS_SNAPSHOT_NRMDUMP=1 \
      ./DEMO ${JUDGE[@]} ${=EXTRA} "$@" --snapshot=greets@t=$t --out="$d" ) > "$d/log.txt" 2>&1
}

# PPM -> PNG, then drop the PPM (6 MB each, and he reads PNGs).
topng() {
  local d=$1
  python3 - "$d" <<'PY' || true
import glob, os, sys
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
for p in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    try:
        Image.open(p).save(p[:-4] + ".png")
        os.remove(p)
    except Exception as e:
        print("ppm2png failed on %s: %s" % (p, e), file=sys.stderr)
PY
}

# delete the raw planes of one arm dir (keeps PNGs, log.txt, *.json).
# null_glob is load-bearing: without it zsh's nomatch aborts the whole `rm`
# on the first pattern that matches nothing, so NOTHING is deleted and the
# lean run silently keeps every plane.
strip_raw() {
  setopt local_options null_glob
  local d=$1
  rm -f "$d"/ref.bin "$d"/*.z16 "$d"/*.u32 "$d"/*.ppm 2>/dev/null
  return 0
}

score() {   # score <posedir>  -- tess vs ref, and tess vs refse if present
  local p=$1
  [[ -f "$p/ref/ref.bin" && -f "$p/tess/log.txt" ]] || return 0
  python3 "$DIFF" --tess "$p/tess" --ref "$p/ref" ${BAREOPT} \
      --out-prefix "$p/ref_vs_tess" --json > "$p/score_ref.json" 2>"$p/score_ref.err" || \
      { echo "  [score] ref arm failed, see $p/score_ref.err" >&2; }
  if [[ -f "$p/refse/ref.bin" ]]; then
    python3 "$DIFF" --tess "$p/tess" --ref "$p/refse" ${BAREOPT} \
        --out-prefix "$p/refse_vs_tess" --json > "$p/score_refse.json" 2>"$p/score_refse.err" || \
        { echo "  [score] refse arm failed, see $p/score_refse.err" >&2; }
  fi
}

for row in $POSES; do
  name=${row%%|*}; rest=${row#*|}; t=${rest%%|*}; cam=${rest#*|}
  if [[ -n "$POSES_WANT" ]] && ! want "$POSES_WANT" "$name"; then continue; fi
  echo "== $name t=$t"
  want "$ARMS_WANT" bare  && run "$OUT/$name/bare"  $t "$cam"
  want "$ARMS_WANT" tess  && run "$OUT/$name/tess"  $t "$cam" --greets-displace
  want "$ARMS_WANT" ref   && run "$OUT/$name/ref"   $t "$cam" --greets_displace_ref --greets_displace_ref_stats=1
  want "$ARMS_WANT" refse && run "$OUT/$name/refse" $t "$cam" --greets_displace_ref --greets_displace_ref_stats=1 --greets_displace_ref_shared_edge

  BAREOPT=""
  [[ -f "$OUT/$name/bare/log.txt" ]] && BAREOPT="--bare $OUT/$name/bare"
  score "$OUT/$name"
  for a in bare tess ref refse; do
    [[ -d "$OUT/$name/$a" ]] || continue
    topng "$OUT/$name/$a"
    (( LEAN )) && strip_raw "$OUT/$name/$a"
  done
  guard "after $name"
done

if (( VARIANTS )); then
for row in "camA|5965|22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \
           "H6194|6194|22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849"; do
  name=${row%%|*}; rest=${row#*|}; t=${rest%%|*}; cam=${rest#*|}
  if [[ -n "$POSES_WANT" ]] && ! want "$POSES_WANT" "$name"; then continue; fi
  echo "== variants $name"
  R="--greets_displace_ref --greets_displace_ref_stats=1"
  run "$OUT/$name/ref_part1"    $t "$cam" ${=R} --greets_displace_ref_partition=1
  run "$OUT/$name/ref_mitre2"   $t "$cam" ${=R} --greets_displace_ref_mitre=2
  run "$OUT/$name/ref_mitre8"   $t "$cam" ${=R} --greets_displace_ref_mitre=8
  run "$OUT/$name/ref_crease15" $t "$cam" ${=R} --greets_displace_ref_crease=15
  run "$OUT/$name/ref_crease60" $t "$cam" ${=R} --greets_displace_ref_crease=60
  for a in ref_part1 ref_mitre2 ref_mitre8 ref_crease15 ref_crease60; do
    topng "$OUT/$name/$a"
    (( LEAN )) && strip_raw "$OUT/$name/$a"
  done
  guard "after variants $name"
done
fi

echo "== disk after the battery"
df -h / | tail -1
echo "out tree: $(out_mb) MiB"
echo "done -> $OUT"
