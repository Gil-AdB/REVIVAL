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
3. ✅ **TBR migration — ALL SCENES SHIPPED** (greets 800fc47, city
   d24bb7f, chase 8c55e80). Every scene's transparents + flares now go
   through the TBR strips (TBR_EnsureInit + Scn_SpriteTBR at scene init,
   gated on --deferred_unified_tbr; --no-… = exact legacy). Holes closed
   on the way: TBR_Sprite got the mirror-clone footprint gate; new
   TBR_MatchesTarget guards all TBR consumers against offscreen passes
   (forward RTT would cross-pollute the main strips); the city A/B
   exposed 3 missing behaviors in TBR_Render's flare draw vs the
   immediate path — fogged colorize, far-plane Z clamp (beyond-FZP
   encoding WRAPS and the flare wins every z-test), Face_PointZTest —
   all ported (fountain byte-identical vs pre-campaign binary).
   Flares now sort by Z — fixes the known "flare over transparents"
   bug. **USER EYEBALL PENDING live**: greets lamp flares through
   glass (t=700), city beacon flares, chase/city water. Perf: greets
   ~1ms faster, city ~0.4ms faster (interleaved; the first city bench
   read as a 7ms regression = machine load — ALWAYS interleave).
   The legacy peel remains ONLY as the offscreen fallback + escape
   flag; deleting it outright needs an offscreen story first.
4. ✅ **#1 RenderContext migration step 4 — DONE. Singleton deleted +
   ALL 9 poisonable render TUs compiler-enforced global-clean**
   (f6519c7..9530b17). Every deferred/post TU now carries
   `#pragma GCC poison XRes YRes VPage ZPage16 FOVX FOVY CntrEX CntrEY
   CurScene VESA_BPSL g_zscale` (Hdr: narrowed — post stack is
   target-polymorphic, fountain's tick drives it outside renderFrame).
   Kernel design unit (5368c78): Render_DeferredLighting resolves via
   MainRenderTargetFromGlobals()/fds::g_mainCamera; ctx.Sc is a CALLER
   CONTRACT (renderFrame + GreetsMirror RTT + MirrorShatter pre-fill);
   policy helpers take Scene*. Pilot caught a real bug (offscreen AA on
   the misaligned main gbuffer). RENDER.CPP stays the sanctioned
   snapshot point BY DESIGN (never poisoned). Every step byte-gated on
   4 scenes + render gate. REMAINING (step 5): per-instance pipelines
   for offscreen parallelism — RTT ~0.9ms serial + cross-light shadow
   raster are the payoffs; the poison guarantees no pass secretly reads
   main state, which is what makes that step safe to attempt.
5. ✅ **#4 mirror system promoted to FDS/RENDER (1e26fc5)** —
   GreetsMirror + MirrorShatter + SpotlightCones moved wholesale (they
   were already parameterized); canonical PickFillerForMaterial now
   lives in TheOtherBarry.h (SceneBuilder delegates). Gate PASS,
   teleporter smoke byte-identical, clean-tree build verified.

Also fixed this session (before the push): lightmap-bake coverage-bit
race (0df96a6, atomic OR — TSan-confirmed real, greets init now clean);
teleporter-stone run-to-run nondeterminism is NOT it — still open,
separate cause. DoF "not working" = user config had --dof_range=20 but
the flag is a FRACTION of far-plane (0.03-0.1 sane); code is fine.
fog-wt merged in (2578dcb): editor write-back, env_refl/PBR (default
OFF), authoring pins. User's local Runtime/SCENES/FOUNTAIN.FLD (80KB)
still shadows the merged pinned 512KB one — user to decide.

## STEP 5 UNIT: parallel mirror-RTT slots — SHIPPED opt-in, residual OPEN (2026-07-04)

STATUS: fan implemented + committed (bdeacfe prepass, 2206d49 HdrTarget
override infra, cbc9220 the fan). DEFAULT SERIAL (byte-exact); the fan is
--mirror-rtt-parallel, OFF, because of an unresolved residual.

RESIDUAL (the one thing left): at t=700 teleporter, the m1->m4 reflected
panel differs from serial ~1600px screen / ~1870px in the 64² slot
(max 58/137), surviving single-worker (FDS_MIRROR_RTT_P1=1, so NOT a race).
DIAGNOSIS (probed 2026-07-04, all via the RTT dump kit + a transient
pre-cone surf-checksum + ov.cam field print):
  * Slot Z16, mat32, normal, lightmapMF, lightmapST planes: byte-IDENTICAL
    to serial. So transform+clip+raster+gbuffer match EXACTLY.
  * ov.cam fields (fovX/fovY/cntrEX/cntrEY/zScale/nearZ/cntrX/cntrY) +
    dctx.viewToWorld[0][0]: byte-IDENTICAL between serial (ov.cam=
    &g_mainCamera) and fan (ov.cam=&w.camCtx).
  * The LIT SURFACE CHECKSUM DIFFERS *before cones run* (pre-cone surf
    f174..bca serial vs 8d63..77e parallel). So the divergence is INSIDE
    Render_DeferredLighting's per-pixel lighting, NOT cones/HDR/tonemap.
  P=1 + same tick thread → thread-locals identical. RULED OUT: geometry,
  camera, all gbuffer planes, cones, HDR resolve, threading, vertex scratch.
  * ViewLightsSoA (ov->lights, all 23 arrays x 117 lights): byte-IDENTICAL
    (hash 75ff00b2b5f17ab6 both). mirrorMask empty in BOTH RTT gbuffers
    (neither s_rttGB nor w.gb allocates it) → clone-cull symmetric.
  CONCLUSION: EVERY enumerable kernel input — 5 gbuffer planes, full
  camera ctx, the entire light SoA — is byte-identical, yet the pre-cone
  lit surface differs. This is NOT a race (P=1, same thread) and NOT any
  enumerable input. It's an uninitialized-read or buffer-provenance
  dependence deep in the kernel tile/fill path (quarter-fill wave-2 reads
  neighbours; s_rttSurf reused-across-jobs vs w.surf reused-per-worker
  carry different residual in never-covered lanes the fill may touch), OR
  a per-pixel codegen/order subtlety. CLOSING IT needs per-pixel
  intermediate bisection (dump wave-1 vs wave-2 pixel masks + the fill's
  neighbour reads for the diverging 36x58 region), a dedicated deep
  session — NOT bounded probes. Given the prize is ~1-1.3ms of ~42ms on a
  DEFAULT-OFF experiment, RECOMMENDATION: leave opt-in, revisit only if
  the RTT serial cost becomes a priority. The fan + HdrTarget infra are
  committed and correct-by-construction; only this residual gates default-on.
Perf prize when closed: ~1.0-1.3ms at ts=491 (serial RTT is 1.77-1.85ms).

--- original design notes below ---


RTT = 1.77-1.85 ms/f SERIAL at ts=491 (biggest reclaimable serial line;
"serial tonemap-post/edge-aa" are internally-threaded, misleading label).
RenderSecondOrderMirrors' per-slot loop transforms the WHOLE scene through
the global camera into the shared face list per slot, then bakes inline.
MirrorShatter's slice-6 ShardWorker already solved this exact shape —
per-worker camCtx (incl. nearZ = D*1.001f mirror clip, MirrorShatter.cpp
~1102), FaceListContext, VertexScratch, surf, gb, lights, tileLights,
inlineDispatch deferred bake. The fan is a transplant:

1. PREPASS (serial): compute adaptive dims + stamp slot UVs
   (sv.v->U/V + face uvFromVertices) for ALL jobs before any render —
   slots view each other's panels, so UV writes must not race worker
   rasters. Byte-identical refactor on its own (UVs don't depend on
   other slots' renders).
2. Per-job worker state (clone ShardWorker or reuse): off-axis camCtx
   from the slot math (the FOVX/CntrE formulas in the loop), faces,
   scratch, per-worker surf sized texWMax², gb, lights/tileLights.
   Transform_Objects(sc, w.camCtx, w.faces) with thread_local
   g_offAxisFrustumCull; Radix_Sort local; MekaleleFillRegionInline;
   Render_DeferredLighting(dctx, &ov) with ov.cam=&w.camCtx,
   inlineDispatch=true (workers ARE pool threads — no nested enqueue);
   cones inline; ctx.Sc = scene (caller contract).
3. HDR SUB-PROBLEM: the slot loop uses Hdr_BeginFramePass /
   Hdr_ActivateNoFog / Render_TonemapToVPage against the GLOBAL
   g_hdrBuf (resized per slot!). Concurrent slots need an explicit
   HdrTarget{buf,W,H,active} param variant of those 3 (+ the cone
   pass's HDR accumulate already goes through ctx? VolCompositeAdd
   reads g_hdrBuf/g_hdrActive globals — check). Global-based versions
   delegate to the param version with the global target = main path
   unchanged. If this balloons, ship increments 1-2 with HDR slots
   still serial (fan the non-HDR case) and leave 3 documented.
4. OffscreenViewScope shrinks to: hide clones / mute flares / restore
   (the world-swap parts die — workers never touch globals).
Validation: per-slot outputs are per-slot textures; transform+bake are
per-worker → byte-identical is achievable. Gate (mirrortest!) + warm
greets t=700/2014 full-config byte A/B + TSan run + the FDS_MIRROR_RTT_DUMP
probe. Payoff est. ~1.0-1.3 ms at ts=491. Note the 2-job/frame cap
memory ("rtt 0.5-0.8ms at the 2-job cap") — if jobs/frame ≤2, measure
whether raising the cap under the fan buys quality headroom for free.

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
