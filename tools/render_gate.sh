#!/usr/bin/env bash
# Byte-identical render gate for the RenderContext migration
# (docs/RENDER_CONTEXT_PLAN.md). Renders the deterministic deferred test
# scenes and compares each pose-set's md5 to a committed baseline. A Slice-3
# step is byte-clean iff every gate PASSes.
#
# Determinism note: these three scenes are stable run-to-run AND
# threaded==serial.
# greets: the old "NOT deterministic (timing-dependent background lightmap
#   bake)" note here was WRONG on both counts. It was never a bake and never a
#   race — it was an 8-bit AO map read as dwords in the deferred kernel, fixed
#   2026-08-05 (docs/SESSION_STATE.md). greets is now 0 flips in 128 runs and
#   IS gate-worthy — but its pin depends on the user's UNCOMMITTED authoring
#   files, so it is gated out-of-band via `tools/flip_rate.sh -n 24` against
#   the pin in SESSION_STATE, not from this script.
# city: verified stable (`tools/flip_rate.sh -s city`), pinned in SESSION_STATE;
#   kept out of here only because of its FLD-keyed envmap cache rebuild.
#
# Usage:   tools/render_gate.sh            # run from repo root (DEMO in Runtime/)
#          tools/render_gate.sh --update   # reprint current md5s (to re-baseline)
#
# Coverage:
#   mirrortest  — deferred surface kernel + mirror clone (NOT the RTT: see below)
#   conetest    — DeferredVolumetric cones + DeferredFastFog (fog on)
#   halotest    — DeferredVolumetric omni halos
#   rttslot     — the ORDER-2 mirror RTT bake, deferred + HDR (2026-08-13)
#
# rttslot exists because the mirrortest row's "+ RTT" claim was false and cost a
# shipped regression. `--scene-mirrortest` alone never enables `mirror_rtt`
# (default 0; only GREETS.CPP's setDefault and the editor turn it on), so no row
# here rendered a single RTT slot. `00d28a8b` dropped the RTT off the HDR path
# (`ctx.hdrBuf = ov ? ov->hdr : …` — the RTT is the DeferredOverride user that
# brings no ov->hdr), changing 98.9 % of the slot's pixels, and mirrortest was
# byte-identical on the broken binary WITH --hdr as well as without. Fixed in
# 283b46ca / verified 6656300b; this row is the coverage that was missing.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/Runtime"
OUT="${TMPDIR:-/tmp}/render_gate"
mkdir -p "$OUT"
cd "$RUN" || { echo "no Runtime/ dir at $RUN"; exit 2; }

# Baselines — committed state. Update with --update after an INTENTED change.
# mirrortest baseline: soa-vertex's d187f9a rebaseline (c340ffa FP-noise drift)
# + ce4f906 mirror clone wall-depth clamp. Verified 2026-07-04 on the merged
# tree — matches soa-vertex's committed golden exactly, so the editor-branch
# work does not perturb mirror output.
# Re-baselined 2026-07-05 for the oct 16.16 normal G-buffer (Mekalele.h):
# measured drift vs prior baseline is <=1 LSB on <1% of pixels (several
# poses byte-identical) — pure precision change, no structural diff.
BASE_MIRROR="4ac809e5f5323076de1a6d5ef2fb9e92"
BASE_CONE="b41894f969d1f89dd2d7d794f160e286"
BASE_HALO="166fa25a846668cc9b2d4dae2d800a7b"
# rttslot baseline: the 4 order-2 slot surfaces mirrortest bakes over its 8 dump
# poses (`/tmp/rtt_m1_m2_{0,1,2}.ppm` 512x512 + `/tmp/rtt_m2_m1_3.ppm` 128x512),
# concatenated. Established 2026-08-13 on 6656300b.
#
# ALL THREE FLAGS ARE LOAD-BEARING — each proved necessary by a control that
# does NOT discriminate 6656300b (fixed) from 00d28a8b (broken):
#   --mirror_rtt      without it 0 slots are prepared and the hash is the md5 of
#                     nothing (d41d8cd9…) on every binary
#   --shard_deferred  without it the slot bakes FORWARD, never touching the
#                     deferred kernel's ctx.hdrBuf — slots a48afe1b… on both
#   --hdr             without it the bug is unreachable — slots 09c9d4d8… on both
# Only the three together separate them: 826c09e6… (fixed) vs 2ecd5e81… (broken),
# 5/5 stable on each. Signature on the broken binary, same as the greets t=3122
# regression: 99.95 % of the COVERED texels change, mean |Δ| 28.5/channel, mean
# luma over the changed texels 157.7 -> 129.9 (−27.8).
#
# WHAT THIS ROW CERTIFIES: that an order-2 RTT slot still bakes through the
# deferred kernel with a live HDR buffer, at mirrortest's two facing mirrors, at
# the adaptive resolutions its 8 poses pick (512x512 / 128x512).
# WHAT IT DOES NOT: greets' own slot geometry (7-8 slots against mirrortest's 2)
# — that stays out-of-band, because a greets row would key on the user's
# UNCOMMITTED authoring files; the FIRST-order RTT panel path (min_area keeps it
# empty in both scenes); the panel composite as it lands in the main frame
# (measured: under --hdr the mirrortest frame is insensitive to slot content —
# it is byte-identical with the forward and the deferred bake — so the SLOT is
# the gated surface here, not the frame); and any RTT change that is invisible
# at these two mirrors, e.g. an adaptive-res policy change that only moves
# greets' sizes. Note the flip side: this row IS sensitive to the adaptive
# resolution mirrortest picks, so an intentional sizing change needs --update.
BASE_RTTSLOT="826c09e63217e778cfcef70fe0167279"

# HEADLESS: every DEMO run uses the SDL dummy driver — no window pops on the
# desktop (the gate gets run a lot, incl. from bisects). Dummy output differs
# from windowed output byte-wise but is deterministic run-to-run, which is all
# a gate needs; the baselines below are DUMMY-mode hashes.
# Audio is dummied for the same reason (no device grab from a bisect loop);
# measured not to move any baseline — mirrortest reproduces 4ac809e5… with it.
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy

md5_of() { ls "$@" 2>/dev/null | sort | xargs cat | md5 -q; }

run_mirror() {
  rm -f /tmp/mt_view_*.ppm
  FDS_MIRRORTEST_MULTI_DUMP=1 ./DEMO --scene-mirrortest >/dev/null 2>&1
  md5_of /tmp/mt_view_*.ppm
}
# The order-2 RTT bake. Same scene as run_mirror, but with the three flags that
# actually reach the path, and the gated surface is the SLOT dump
# (FDS_MIRROR_RTT_DUMP=1 -> /tmp/rtt_*.ppm, hardcoded in GreetsMirror.cpp), not
# the frame. MULTI_DUMP is still what makes the driver headless-exit; its
# mt_view PPMs are written and ignored here (run_mirror clears them itself).
run_rttslot() {
  rm -f /tmp/rtt_*.ppm /tmp/mt_view_*.ppm
  FDS_MIRRORTEST_MULTI_DUMP=1 FDS_MIRROR_RTT_DUMP=1 \
    ./DEMO --scene-mirrortest --mirror_rtt --shard_deferred --hdr >/dev/null 2>&1
  md5_of /tmp/rtt_*.ppm
}
run_cone() {
  rm -f "$OUT"/conetest_*.ppm
  ./DEMO --snapshot=conetest --out="$OUT" --deferred --draw_cones --shadows \
         --fast_fog --fast_fog_density=3 >/dev/null 2>&1
  md5_of "$OUT"/conetest_*.ppm
}
run_halo() {
  rm -f "$OUT"/halotest_*.ppm
  ./DEMO --snapshot=halotest --out="$OUT" --deferred --omni_halo_strength=0.5 >/dev/null 2>&1
  md5_of "$OUT"/halotest_*.ppm
}
# INVARIANT (not a fixed golden): the second-order mirror RTT must render
# IDENTICALLY whether the deferred bakes run serially or fanned across the
# pool (--mirror-rtt-parallel). Uses the FDS_MIRRORTEST_SPOT forced-cone
# beam so the RTT reflection contains a volumetric cone — the exact case
# the tileChunkSphere-reads-globals bug corrupted (serial worked by
# accident; the fan used the main camera's projection → mis-culled spots).
# Regression guard for commit 54a9d50.
# NB: the serial leg MUST force FDS_MIRROR_RTT_SERIAL=1 — mirror_rtt_parallel
# defaults ON since 3cf9456, so a flagless run is parallel and the check
# would silently compare parallel to parallel.
run_rtt_parallel_invariant() {
  local ser par
  rm -f /tmp/mt_view_*.ppm
  FDS_MIRROR_RTT_SERIAL=1 FDS_MIRRORTEST_SPOT=1 FDS_MIRRORTEST_MULTI_DUMP=1 \
    ./DEMO --scene-mirrortest \
    --mirror-rtt --shard-deferred --hdr >/dev/null 2>&1
  ser=$(md5_of /tmp/mt_view_*.ppm)
  rm -f /tmp/mt_view_*.ppm
  FDS_MIRRORTEST_SPOT=1 FDS_MIRRORTEST_MULTI_DUMP=1 ./DEMO --scene-mirrortest \
    --mirror-rtt --shard-deferred --hdr --mirror-rtt-parallel >/dev/null 2>&1
  par=$(md5_of /tmp/mt_view_*.ppm)
  [ "$ser" = "$par" ] && echo "MATCH" || echo "MISMATCH(ser=$ser par=$par)"
}

M=$(run_mirror); R=$(run_rttslot); C=$(run_cone); H=$(run_halo)
RTTP=$(run_rtt_parallel_invariant)

if [ "${1:-}" = "--update" ]; then
  echo "mirrortest: $M"
  echo "rttslot:    $R"
  echo "conetest:   $C"
  echo "halotest:   $H"
  exit 0
fi

rc=0
chk() { # name actual baseline
  if [ "$2" = "$3" ]; then echo "  PASS  $1  $2"
  else echo "  FAIL  $1  got $2  want $3"; rc=1; fi
}
echo "render gate:"
chk mirrortest "$M" "$BASE_MIRROR"
chk rttslot    "$R" "$BASE_RTTSLOT"
chk conetest   "$C" "$BASE_CONE"
chk halotest   "$H" "$BASE_HALO"
chk rtt-parallel "$RTTP" "MATCH"
[ $rc -eq 0 ] && echo "ALL PASS" || echo "GATE FAILED"
exit $rc
