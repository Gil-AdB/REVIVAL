# SESSION STATE — mirror/xpar cost campaign (updated 2026-07-03, end of marathon session)

Read this when resuming. Branch feature/soa-vertex @ 5b71911, all pushed.
Perf floor at the ts=491 reference: **~40.4 ms min** (session start was 43.4;
campaign start ~51). Bench recipe + load-sanity rules live in the memory file
`greets-bench-reference-frame` and docs/FRAME_PIPELINE_PLAN.md.

## NEXT TASK (user-approved "go", deferred to a fresh head): xpar composite fast path

The mirror cost anatomy is fully measured (FRAME_PIPELINE_PLAN.md 2026-07-03
entry): full-screen mirror = +18 ms, and the xpar peel's share is 5.1 ms which
decomposes (new FDS_TAIL_PROF `xpar-phases` line, commit 5b71911) as:

    clear 0.39 + raster 0.73 + COMPOSITE 2.94 + ~1.0 serial mirWin scan

Reference pose for all of this work:
    FDS_GREETS_CAM='0.0,2.5,-2.5,0.0,0.0,1.0'  --bench=scene@scene=greets,ts=700
    (teleporter mirror fills the screen; mirror on/off A/B = --greets-mirror /
    --no-greets-mirror; full user config in FRAME_PIPELINE_PLAN.md)

Two concrete sub-targets, in order:

1. **The composite (2.94 ms)** = renderDeferredTransparentTile_Front/Back →
   the transparent lighting kernel (DeferredSurfaceKernel.cpp xpar peel
   template) running the FULL per-pixel light loop over every covered pixel,
   3 batches/frame at the pose (clone back + clone front + portal glass).
   Options considered:
   a. Quarter/checker rate for the xpar composite (mirror content tolerates
      it; needs a fill pass like the opaque C-fill — biggest but most work).
   b. Cheap-glass path: the half-silvered glass material has Diffuse=0
      (GreetsMirror.cpp ~614-668: lit = Lum*255, silver tint) so the light
      loop contributes ONLY specular there — a spec-only (or skip-loop)
      branch for Diffuse==0 materials is small and targeted, but only covers
      the glass batch, not the clone batches.
   c. Cap/cull lights harder for clone-tagged xpar pixels.
   Recommendation: measure per-batch composite cost first (extend the
   xpar-phases print with per-batch tag/ms) to see if the glass batch or the
   clone batches dominate, THEN pick a/b/c. Don't skip this step.

2. **The ~1 ms serial mirWin scan** (RENDER.CPP peel preamble ~line 680:
   full-screen gb.mirrorId byte scan building per-mirror bboxes). Either
   parallelize (dispatchIndexed row bands + per-band bbox merge) or derive
   from StampMirrorMasks' stamp loop (it already touches exactly those
   pixels — accumulate bboxes there for free).

## Validation kit for that work
- tools/render_gate.sh (now HEADLESS — SDL_VIDEODRIVER=dummy inside) must
  stay ALL PASS; it covers the xpar kernel via mirrortest.
- The t=700 teleporter pose above: fixed-vs-variant snapshot must be
  zero-pixel for gated-off, eyeball for quality changes.
- Warm-sequence repro rule: mirror state accumulates across the timeline —
  cold snapshot jumps show NOTHING (clone-lanes=0). Use
  --snapshot=greets@t=1800,1850,...,2014 style sequences.
  FDS_MIRROR_CLAMP_STATS=1 prints clone-lanes/rejected per frame.
- Machine load creeps mid-session: re-run a known config (quarter+C ts=491 =
  ~40.4 idle) before trusting any new number; interleave A/B variants.

## What shipped this session (all pushed, see git log 44fdef5..5b71911)
- HDR regressions (f16 clamp, bright-pass revert, C-fill trust region).
- checkerC greets default + C ported to checker fill (NOTE: real cost vs
  quarter is +6 ms at ts=491 — the +0.5-1 reading was load-contaminated;
  user runs --deferred-quarter explicitly; default flip-back is one line in
  GreetsApplyRunDefaults, still pending a user decision).
- Half-res DoF (default), moving-omni-128 default, bake mesh cache,
  temporal SSAO (--ssao_temporal, default off, user now runs it).
- Frame-pipeline verdict: DON'T build tick∥render (ceiling ~3 ms, bandwidth
  kills overlaps — 3 variants measured dead). Parallel Lighting shipped.
- dispatchIndexed (Threads.h): work-stealing chunk dispatch engine-wide,
  −3 ms; TWO lifetime traps documented in its comments (temporary fn dies
  before the drain → copy fn by value; thread_local vectors re-resolve on
  the worker inside [&] lambdas → snapshot .data()).
- P-core-only scheduling measured: LOSES (~+2.7 ms); E-cores net-positive.
- Profiler prints top-10 slowest frames' scene-t (`slowest frames (ms@t)`).
- Mirror-through-wall moire FIXED (ce4f906): clone wall-depth clamp
  (z_candidate vs mirrorMaskZ+16). FDS_NO_MIRROR_WALLZ_CLAMP=1 = A/B escape.
- render_gate.sh headless + mirrortest golden rebaselined (was silently red
  since c340ffa — run the gate at session start).
- ASan finds: SceneCorrections stack over-read (bounds-guarded).

## Known open items beyond the xpar task
- User to eyeball live: temporal SSAO mech-contact-AO lag; f16/DoF look.
- quarter-vs-checker default decision (above).
- RTT pass (1.68 ms serial) blocked on RenderContext migration tail
  (RENDER_CONTEXT_PLAN.md) — the migration's first payoff when taken up.
- x86 measurements (vec light loop default ON there, never measured).
