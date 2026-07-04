# SESSION STATE — structural push (updated 2026-07-04)

Read this when resuming. Branch feature/soa-vertex, all pushed.
Perf floor at the ts=491 reference: **~40.4 ms min** on an idle box
(machine-load drift makes day-to-day mins swing 41-44; always interleave
A/B). Bench recipe + load-sanity rules live in the memory file
`greets-bench-reference-frame` and docs/FRAME_PIPELINE_PLAN.md.

## STRUCTURAL PUSH (user-approved campaign, 2026-07-04) — status

Ranked list (full reasoning in the 2026-07-04 conversation + below):
1. ✅ **#3 dispatch consolidation (e1355f0)** — every pool fan now goes
   through dispatchIndexed (was: task-per-tile loops, hand-rolled
   cursor+semaphore fans, TBR_Render's mutex+condvar). Net −76 lines.
   Validated: gate ALL PASS, city/fountain snapshots byte-identical,
   greets diff == pre-existing noise floor, TSan clean, perf-neutral.
2. ✅ **#2 binning helper (c786b1f)** — lightSphereScreenRect in
   DeferredCommon.h replaces the 4 copied sphere→tile-range blocks
   (kernel/strips/cones/halos; the clone-cone cull gap was exactly this
   rot). zeroTileLightPadding folds the two padding blocks. Same
   validation, all green.
3. 🟨 **TBR migration — greets slice SHIPPED (800fc47)**; city + peel
   deletion remain. Greets now routes xpar + flares through the TBR
   strips (TBR_EnsureInit + Scn_SpriteTBR in GreetsScene::init, gated on
   --deferred_unified_tbr; --no-… = exact legacy). Two holes closed:
   TBR_Sprite got the mirror-clone footprint gate; new TBR_MatchesTarget
   guards all TBR consumers against offscreen passes (forward RTT would
   have cross-polluted the main strip lists). Flares now sort by Z —
   FIXES the known "flare over transparents" bug (eyeballed at t=700:
   mirror content identical, lamp flares attenuate through glass —
   USER EYEBALL PENDING live). ~1ms faster at ts=491 + the event window.
   Remaining: city (rain-particle + water interactions — own slice);
   the legacy peel can't be DELETED (offscreen fallback uses it).
4. ⬜ **#1 RenderContext migration slices 3-5** (RENDER_CONTEXT_PLAN.md) —
   the standing campaign; unlocks RTT/shadow-raster parallelism, ends the
   global-swap bug family.
5. ⬜ **#4 promote GreetsMirror/MirrorShatter to FDS/RENDER** — mechanical.

Also fixed this session (before the push): lightmap-bake coverage-bit
race (0df96a6, atomic OR — TSan-confirmed real, greets init now clean);
teleporter-stone run-to-run nondeterminism is NOT it — still open,
separate cause. DoF "not working" = user config had --dof_range=20 but
the flag is a FRACTION of far-plane (0.03-0.1 sane); code is fine.
fog-wt merged in (2578dcb): editor write-back, env_refl/PBR (default
OFF), authoring pins. User's local Runtime/SCENES/FOUNTAIN.FLD (80KB)
still shadows the merged pinned 512KB one — user to decide.

## CONES/CLONES CAMPAIGN — DONE 2026-07-04 (commit 5a58269)

The t=1130-1162 cluster + attack results (full numbers in
FRAME_PIPELINE_PLAN.md 2026-07-03/04 entries):
1. **Clone-cone footprint cull SHIPPED**: 40/50 cone spots were mirror
   clones with no tile-level footprint cull. Cones wall 6.4-7.5 → 3.6-4.1
   in-window (p50 −4 ms), ts=491 −1.3 ms. Byte-identical + gate PASS.
   Debug: FDS_NO_CONE_MIRROR_CULL / FDS_CONE_SKIP_* / FDS_CONE_ATTR.
2. **Clone lights in the kernel: measured DEAD** — presence cull already
   contains them (w1 10.0 vs 10.1 mirror off/on, avg lights/tile ~10.5
   both). Don't build a cap. Remaining mirror delta (+2.5 min/+4 p50) is
   spread machinery (peel 1.3-1.5, RTT 0.9, masks 0.8, clone raster).

## PREVIOUS NEXT-TASK (deprioritized 2026-07-03): xpar composite fast path

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
   MEASURED (per-batch print landed): the GLASS batch (tag=0 front, 4 faces)
   is 2.645 ms of the 2.94 composite; clone batches are 0.30 combined. So
   option (a) is DEAD (don't build quarter-rate xpar) and (b) is the task:
   cheap path for Diffuse==0 glass — either skip the light loop entirely
   (if the spec sheen isn't load-bearing — EYEBALL at the t=700 pose, gates
   can't judge it) or a spec-only loop (skips diffuse+shadow work). ~2.5 ms
   at full mirror coverage; ~0 at ts=491 (coverage-scaled).

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
