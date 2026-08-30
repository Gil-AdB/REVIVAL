#!/usr/bin/env bash
# WARM multi-tick gate — the coverage the 13 snapshot pins STRUCTURALLY cannot
# provide (docs/SESSION_STATE.md gates table; docs/OPTIMIZATION_BACKLOG.md
# 2026-08-29c).
#
# WHY THIS EXISTS. Every row of the canonical gate suite is a ONE-TICK
# `--snapshot`. A path that only becomes reachable on the SECOND tick of a
# process therefore executes zero times in the whole suite, and a green 13/13
# says nothing whatever about it. That is not hypothetical: on 2026-08-29 the
# froxel composite's water-reflection punt — 27.6 % of city's groups and half
# of `fog-composite` — was rewritten, and all 13 pins passed before AND after
# because `FastFog_SetReflectionZ` has nothing to hand over until city's water
# carries mirrored content, which it does not on tick 1. The change was gated
# on the recipes below instead.
#
# WHAT EACH ROW COVERS THAT A SNAPSHOT ROW CANNOT:
#   city-warm        the water-reflection leg of the froxel composite
#                    (gFrReflZ / Froxel_ReflBranch / --fog_refl_vec), the
#                    froxel temporal EMA's blend arm (gFrHistValid), city's
#                    dispMap wobble and mirrored-water content
#   greets-warm      greets' iterative code-screen smear (OldBuf feedback —
#                    a function of how many times Render() has run, not of t)
#   chase-warm       chase's reflection pass across ticks
#   city-warm-plain  the same, without --city_env_pixel / --env_live_water
#   city-warm-hdr    the HDR composite output stage on warm reflective lanes
#   city-warm-dim    the legacy distance-dim multiply on warm reflective lanes
#   city-warm-noal8  the SCALAR composite tile path (tail loop reachable)
#
# FAST vs FULL: with no argument this runs the two rows that cover the holes we
# have actually been bitten by (city-warm, greets-warm) — ~1 min. `--full`
# runs all seven. Run the fast set on every gate check; run --full before
# merging anything that touches the composite, the froxel volume, or reflections.
#
# RESOLUTION: PASSED EXPLICITLY, NEVER READ FROM THE TREE (2026-08-30).
# These baselines are 1920x1080 dummy-mode hashes. They USED TO be unable to
# pass in a tree whose Runtime/rev.cfg carried the owner's window size
# (1384x768) — the same false red that fired on render_gate.sh three times in
# two days. Every row now carries --force_xres/--force_yres (FeatureFlags.def;
# applied in REV.CPP right after parseArgs, byte-null at their 0 default),
# which win over rev.cfg, so this gate is cfg-independent and runs anywhere.
# CAVEAT for WARM_GATE_BIN: a binary built before 2026-08-30 does not know
# those flags and --strict_flags will abort it (exit 2) — the row then reports
# ERROR ... 0/N frames, not a hash mismatch.
#
# BASELINES: recorded at bc36387b and UNCHANGED SINCE. They have survived one
# real regression, and the way that went is the standing lesson:
#
#   On 2026-08-29 city-warm and city-warm-noal8 went red at ticks 4-5 (ticks
#   1-3 and all 13 one-tick pins byte-identical). The divergence was ONE PIXEL
#   at max |d| 1-2 and deterministic 3/3, so it looked exactly like the
#   accepted-class LSB drift this tree re-pins for all the time -- and it was
#   RE-BASELINED here for about ten minutes. It was not drift. It was a broken
#   per-mesh bsWorld cache in another agent's city glass fan-out, which its
#   owner then found and removed; the fix restored these exact values.
#
# **A WARM-GATE RED FROM SOMEONE ELSE'S CHANGE IS NOT YOURS TO RE-BASELINE.**
# Report it to the owner and leave the baseline alone. "One pixel, 1 LSB,
# deterministic" is not evidence of harmlessness -- a stale cached bounding
# sphere presents exactly that way, and re-baselining would have blessed it
# permanently.
#
# Usage:  tools/warm_gate.sh [--full] [--update]
#         WARM_GATE_BIN=/path/to/DEMO tools/warm_gate.sh --full
#           (the binary still resolves its OWN asset root, so point it at a
#            build whose ../../Runtime is a stock-rev.cfg tree)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/Runtime"
OUT="${TMPDIR:-/tmp}/warm_gate"
FULL=0; UPDATE=0
for a in "$@"; do
  case "$a" in
    --full)   FULL=1 ;;
    --update) UPDATE=1 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
RES_ARGS="--force_xres=1920 --force_yres=1080"   # see the resolution note above
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$RUN" || exit 1
BIN="${WARM_GATE_BIN:-./DEMO}"

CITY_ARM="--env_live_water --deferred --city_env_pixel"
GREETS_ARM="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0"

names=(city-warm greets-warm)
recipes=(
  "--snapshot=city@t=1958,1959,1960,1961,1962 $CITY_ARM"
  "--snapshot=greets@t=5739,5740,5741,5742,5743 $GREETS_ARM"
)
expect=(
  "aa45e9a6ad3eb41c7ec7d9e2976b2a33 2c56ce520da2123b0a8b0635dcaa447f de6db0e3e2bcefe5b6b7583f5da20e37 cb4db07edad634ec4f224134c0040a0a 540ae440ddadc2b815349f8e66503f51"
  "2d652283d04a9e286f3954726322c13f d3279eb566580442e6d41dba402c0bf8 b065d0fa9fc931b6c051c86636848086 fd82a58d895a468ee064dd26bad0663e c0266682a6b8a63839d1db4fdcf2d8a4"
)
if [ "$FULL" = 1 ]; then
  names+=(chase-warm city-warm-plain city-warm-hdr city-warm-dim city-warm-noal8)
  recipes+=(
    "--snapshot=chase@t=1100,1200,1300,1400,1500 --deferred"
    "--snapshot=city@t=2200,2300,2400,2500,2600 --deferred"
    "--snapshot=city@t=1000,1100,1200 $CITY_ARM --hdr --hdr-linear"
    "--snapshot=city@t=1500,1600,1700 $CITY_ARM --fast_fog_dist_dim=0.5"
    "--snapshot=city@t=1959,1960,1961 $CITY_ARM --no-fog_composite_tile_align8"
  )
  expect+=(
    "05ae310bab93db90cde8049b477a2a45 e2d1a87b73fbb2df2c60f6b37ae467f5 3b5b72114a546a241518accdac4b1f04 d7235d9b98a666d6a9aa9b1f93de10dc 6d4a45ec19754e6a755f61d4ab95e825"
    "136a758c7c4436ddc4694140a2b5d60a 9a1d34e02100b8f645ab02074ddf2fb1 c89ea48f084ca87ece699c6c163e5625 9b88d6c5aecb2cb7303aca5b1d24ca46 e263aa8e0c1bb4a3200ce251cff8db3d"
    "5961ad8bfe016215129efdac7279421d 083c44a11ea38d033fe1ccf9e9a84ade 720ee4990865dca20f60b07752ce7603"
    "d2b75240759f8f99ec54f7f84957a05f 0119be58c585e8f0a383dd95876b618d 1681829b4d3608a1083e809f9663fb32"
    "b5de2a662a584d890e564de004091070 37ec05849727bc124e64459172a95390 8f249cb05d8a291c5820f4f87cab92c7"
  )
fi

PASS=0; FAIL=0
echo "warm gate ($([ "$FULL" = 1 ] && echo full || echo fast)):"
for k in "${!names[@]}"; do
  n="${names[$k]}"; d="$OUT/$n"; mkdir -p "$d"
  # shellcheck disable=SC2086
  "$BIN" ${recipes[$k]} $RES_ARGS --out="$d" >"$d/.stdout" 2>"$d/.stderr"
  rc=$?
  got="$(md5 -q "$d"/*_color.ppm 2>/dev/null | tr '\n' ' ')"
  got="${got% }"
  nppm="$(ls "$d"/*_color.ppm 2>/dev/null | wc -l | tr -d ' ')"
  want_n="$(printf '%s' "${expect[$k]}" | wc -w | tr -d ' ')"
  if [ "$UPDATE" = 1 ]; then printf "  %-18s %s\n" "$n" "$got"; continue; fi
  # A row that produced no/too-few frames is an ERROR, not a hash mismatch, and
  # must say so: an empty `got` printed as a FAIL reads like a pixel regression
  # and sends the next reader hunting one. (Cost this suite an hour on
  # 2026-08-29.)
  if [ "$rc" != 0 ] || [ "$nppm" != "$want_n" ]; then
    printf "  ERROR %-18s ran but produced %s/%s frames (exit %s) -- NOT a pixel mismatch\n" \
      "$n" "$nppm" "$want_n" "$rc"
    printf "        stderr tail: %s\n" "$(tail -2 "$d/.stderr" 2>/dev/null | tr '\n' ' ' | cut -c1-160)"
    FAIL=$((FAIL+1))
  elif [ "$got" = "${expect[$k]}" ]; then
    printf "  PASS  %-18s %s\n" "$n" "$got"; PASS=$((PASS+1))
  else
    printf "  FAIL  %-18s\n        got  %s\n        want %s\n" "$n" "$got" "${expect[$k]}"; FAIL=$((FAIL+1))
  fi
done
[ "$UPDATE" = 1 ] && exit 0
if [ "$FAIL" = 0 ]; then echo "ALL PASS ($PASS/$PASS)"; else echo "$FAIL FAILED ($PASS passed)"; exit 1; fi
