# SESSION STATE — structural push (updated 2026-07-04)

Read this when resuming. Branch feature/soa-vertex, all pushed.

## WORKDIR MOVE (2026-07-04): continue from the MAIN tree

The user moved off the gbuffer worktree. Continue from
`/Users/gil-ad/work/revival` (main tree, now on feature/soa-vertex,
built). The gbuffer worktree (`.claude/worktrees/gbuffer`) is DETACHED
and parked — its untracked artifacts (build dirs, scene experiments,
Runtime/tasks_artifacts) are still there if needed; safe to
`git worktree remove --force` once nothing in it is missed. fog-wt is
untouched. Run `tools/render_gate.sh` at session start (ALL PASS as of
the move).

## RESOLVED 2026-07-04 (9902349): chase water dark band — two stacked defects

USER EYEBALL PENDING live; headless verified by images (t=1000/1500,
--deferred and --hdr): water back to forward-reference bright blue, no
band. Root causes (NOT the earlier hypotheses):

1. TBR regression (8c55e80): InsertTransparentToTBR computed its strip
   span from projected PY, garbage for verts at/behind the near plane.
   Chase's giant un-tessellated water quad (4 verts, always camera-
   straddling, near verts at tz≈-130k) inserted only into strips ABOVE
   the horizon → the water NEVER RENDERED under --deferred. The visible
   "water" was the pass-1 reflection underlay + RenderGlints; the band
   was the edge of the mirrored-mountain content (pitch-tracking,
   translation-fixed — matched every symptom). Fix: straddling faces
   insert into ALL strips (per-strip clipper trims), renderZ = far
   surface so flares still composite on top. Same fix brightened city's
   deferred bottom-edge water rows (they straddle too; brighten-only,
   max delta 24 — the only ref-vs-new diff on deterministic frames).
2. The ORIGINAL band (pre-TBR, seen via --no-deferred_unified_tbr):
   water_procedural's kernel composite (xpar kernel ~2100-2270, NOT the
   opaque isWater at 1092 — water is Mat_Transparent!) replaces the lit
   texel with wDeep*(1-fresnel) + underlay*fresnel. City-tuned: assumes
   a BRIGHT reflection underlay. Chase reflects black night sky → dark
   water + view-angle fresnel cut. Fix: new flag water_fresnel_composite
   (default ON = city unchanged); chase factory defaults it OFF → chase
   keeps procedural ripple/glints/caustics but uses the forward-formula
   lit-texel + underlay blend (byte-matched shape: texel*l/256 + dst*dw).
   City factory re-pins it ON for replay ordering.

Diagnosis trail that worked: forward vs deferred pixel sampling → water
vertex-light stats (flat 128 from static bake = ambient) → xpar kernel
debug print NEVER FIRED → walked the drop upstream (kernel → clump →
strip insertion → PY garbage). The earlier "pinned at the isWater
composite" note was wrong — chase water never reaches the OPAQUE kernel.

env_refl-on-water follow-up: RESOLVED, NO CODE CHANGE (2026-07-04,
measured). env_refl has exactly ONE application site — the OPAQUE
kernel compose (envP ~1136/1675); the xpar kernel has none. Probes
confirmed ZERO water pixels reach the opaque or C-fill kernels in
chase OR city, full-res or quarter (all water is Mat_Transparent →
xpar kernel; the opaque/fill isWater composites are dead code for
current scenes, left in place). So env never applies to water pixels
directly — the earlier `&& !isWater` line-1136 exclusion was a no-op
on real water, and its observed "greying" was mis-attributed during
the water-not-rendering state. The env "tint on water" is indirect:
env changes the mirrored geometry in the pass-1 underlay and the
water correctly reflects it (chase ±env_refl diff is confined to the
mirrored-mountain / reflected-flare regions with a clean zero-diff
gap between them — NOT a uniform water tint).

Chase deferred water vs forward residual: deferred samples the water
texture at rasterizer mip (crisper caustic web, slightly brighter);
eyeball live before tuning anything.

## REVIEW PASS 2026-07-04 (post-RTT-campaign audit) — all claims re-verified

Independent re-verification of the RTT campaign: 54a9d50 fix genuine
(all 3 tileChunkSphere call sites ctx-fed, poisoned TUs), TRUE-serial
(FDS_MIRROR_RTT_SERIAL=1) vs parallel byte-identical with the cone spot,
spot non-vacuous, TSan 0 warnings at HEAD, no sneaky gate re-baselining
(d3a06fb came from 348216c's documented dummy-driver switch). ONE real
defect found+fixed (6e64abe): 3cf9456's default-ON flip made the gate's
flagless "serial" leg run PARALLEL — the rtt-parallel invariant was
comparing parallel to parallel. Serial leg now forces
FDS_MIRROR_RTT_SERIAL=1. Lesson recorded: a default flip silently
vacuates any A/B gate whose legacy leg is the flagless run — pin gate
legs explicitly.
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

## STEP 5 UNIT: parallel mirror-RTT slots — DONE + DEFAULT ON (2026-07-04)

SHIPPED, default-on, byte-identical to serial. RenderSecondOrderMirrors'
deferred RTT bakes fan across the pool (ShardWorker pattern: per-worker
camCtx/faces/scratch/gb/lights/tileLights/HDR target). Commits:
bdeacfe (prepass hoist), 2206d49 (HdrTarget override), cbc9220 (fan),
54a9d50 (THE fix), fc66664 (mirrortest cone + gate invariant),
3cf9456 (default-on).

ROOT CAUSE (the "wrong with doubly-reflected mirrors + cones" bug, user-
pinned): tileChunkSphere() — used ONLY by the spot-cone cull, in
buildTileLightLists AND the volumetric cone binning — read the GLOBAL
FOVX/FOVY/CntrEX/CntrEY instead of the render camera. Serial stamped
those globals from the RTT camera (correct by accident); the fan uses
per-worker camCtx and never touches globals → tileChunkSphere used the
MAIN camera's projection → cone cull mis-culled spots in the reflection
(1592 vs 1760 light-tile entries). FIX: pass the projection explicitly.
Companion: buildTileLightLists' chunk[] was a file-static → cross-worker
race; made per-call local. Diagnosis lesson: every PASSED input was
byte-identical (gbuffer/camera/lightSoA/depth) — the bug was a global
read that bypassed the parameters, invisible to input-checksumming; the
tileLights-output checksum (1760 vs 1592) + the cone hint cracked it.

VALIDATION: P=1 byte-exact; N-worker deterministic + serial-matching (7
runs); warm corridor t=1800/1900/2014 exact vs FDS_MIRROR_RTT_SERIAL;
TSan-clean; render gate 'rtt-parallel' invariant (FDS_MIRRORTEST_SPOT
forced-cone beam in the RTT reflection, non-vacuous). PERF: serial RTT
~1.9→1.7ms ts=491, ~1.0→0.7ms event window (scales with panel count).
FDS_MIRROR_RTT_SERIAL=1 = A/B escape.

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

## Headless snapshots now run-to-run byte-identical (d8cd6e2, 2026-07-04)

The fountain/city/greets snapshot "nondeterminism" that forced A/B
comparisons to discard frames was ENTIRELY the on-screen profiler text
(wall-clock FPS/ms). initSnapshotEnvironment's g_profilerActive=0 was
being silently re-armed every frame by FrameProfiler::beginFrame's
flag mirror. New `profiler_overlay` flag (default ON, snapshots
setDefault OFF, --profiler_overlay forces back) gates drawOverlay +
the per-scene FPS printers; stats/dump/bench output unchanged.
Verified: fountain all frames, greets t=700/1500, city t=5000 rr
byte-identical; zero scene-pixel change vs pre-change binary.
NOTE: the greets teleporter-stone run-to-run item may have been this —
re-test it before investigating further.

FOLLOW-UP SHIPPED (c0347f4): `screen_text` master HUD kill-switch. The
remaining UNCONDITIONAL counters (crash frame counters, city t=
indicator, greets Shadow:[F3] indicator, fountain follower-omni labels,
SDL2 flip resolution overlay) are now behind it; snapshot mode
setDefaults it OFF (captures = scene pixels only), --no-screen_text
removes all HUD at runtime, --screen_text forces it back headless.
Debug-viz probes (env-gated) + mirrortest labels (golden-hashed)
deliberately untouched. NOTE for A/B vs OLD reference captures: text
regions will diff once — re-baseline references, or compare new-vs-new.

## Known open items beyond the xpar task
- User to eyeball live: temporal SSAO mech-contact-AO lag; f16/DoF look.
- quarter-vs-checker default decision (above).
- RTT pass (1.68 ms serial) blocked on RenderContext migration tail
  (RENDER_CONTEXT_PLAN.md) — the migration's first payoff when taken up.
- x86 measurements (vec light loop default ON there, never measured).
