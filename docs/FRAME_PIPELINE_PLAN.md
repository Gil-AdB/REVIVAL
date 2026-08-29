# Frame pipelining — Slice-0 measurement + verdicts (2026-07-03)

The campaign from ARCHITECTURE_NEXT §3.6 ("tick N+1 ∥ render N — the big Amdahl
door"). Slice 0 measured the actual prize before building the high-risk
machinery, and the numbers reshape the whole campaign.

## Slice 0 — where the serial time actually is (greets ts=491, quarter, full config)

`FDS_TAIL_PROF=1` tick-thread decomposition (Tick-* marks in GREETS.CPP):

| pure-serial pre-render chunk | ms/f |
|---|---|
| Tick-Anim (Dynamic_Camera + Animate_Objects) | 0.03 |
| Tick-Logic (shatter / omni clones / disco / blaster) | 0.12 |
| RTT (second-order mirror pass) | **1.68** |
| Tick-Xfrm (main Transform_Objects) | 0.44 |
| Tick-Light (forward vertex Lighting) | 0.83 |
| Tick-Radix | 0.02 |
| **pipelinable total** | **~3.1** |

**The "24-28% pool idle" does NOT mean a 25% pipelining prize.** It decomposes
into: (a) this ~3.1 ms serial tick (7% of the 43.4 ms frame — the ONLY part
cross-frame pipelining can reclaim), (b) the shadow bake (pool-parallel
already; overlapping it measured net-negative twice — Stage A and P2 below),
(c) in-render orchestration gaps (lighting-orchestrator serial ~1.75 ms,
StampMasks 0.31, activate 0.21, xpar-peel partial) — reachable only by
parallelizing each section, not by pipelining (one tick thread's work can't
fill a 12-worker idle gap), and (d) already-threaded wall time that the
ScopeTimer labels "serial" misleadingly (tonemap-post 1.69, edge-aa 0.78 are
threaded passes).

## What shipped from this

**P1 — parallel vertex Lighting (`--vertex_light_parallel`, default ON;
the flag was **deleted 2026-08-29** and the fan is unconditional — the arms were
byte-identical by construction).**
Lighting() extracted to a per-mesh body + fanned across the pool. TWO lessons:
- Per-mesh tasks REGRESSED (1.27 ms vs 0.83 serial): greets is many small
  wall-chunk meshes; enqueue/semaphore overhead drowned the work.
- Chunked (12 strided tasks): 0.83 → 0.49 ms, frame ~−0.3 ms. Only 1.7× from
  12 workers — the pass is bandwidth-bound, consistent with the engine-wide
  finding. Byte-identical (gate ALL PASS; per-mesh math untouched).

**P2 — bake-over-tick overlap (`--bake_tick_overlap`, default OFF — measured
NO-WIN, kept for reference).** Dispatch the dynamic shadow bake before the
serial Transform/Lighting/Radix chunk (complementary resources: pool tasks
under a serial tick thread), explicit tick-thread join before Render. Wired
via the Stage-A machinery (ShadowBake_DispatchGreets thread + JoinPending),
guarded against the shatter window (fresh shard meshes would race their first
clone-init copy) + warmup. Correctness verified: pixel-identical ON vs OFF and
deterministic across runs (mask the profiler overlay + the teleporter stone).
Perf: ~+0.3-0.8 ms WORSE in every configuration, including with serial
Lighting (its designed window). The overlap window (~1.3 ms serial) is real,
but Transform/Radix and the bake raster contend on MEMORY BANDWIDTH, not
cores — the same physics that killed Stage A kills this smaller version.

**Gate rebaseline:** tools/render_gate.sh mirrortest golden had been red since
c340ffa (2026-06-18, SIMD'd gradient setup — commit documents FP-noise edge
pixels; the bump was forgotten). Bisected mechanically, rebaselined at
today's hash; every commit of the last two weeks produces the identical hash.

## Verdict on the full cross-frame pipeline

After P1, the remaining pipelinable serial is ~2.6 ms: RTT 1.68 + Xfrm 0.44 +
Light 0.49. Building the full tick-N+1 ∥ render-N machinery (double-buffered
FaceListContext + VertexScratch clones for the main pass + camera copy +
light-snapshot hoist + shatter/TBR hazard audit — the highest-risk change in
the codebase per THREADING_OVERLAP_PLAN) buys AT MOST that 2.6 ms, and the P2
result says bandwidth contention would eat part of it. **Not worth building
until something changes.** The honest residual paths, in order:

1. **RTT (1.68 ms) is the real target.** It is blocked on the RenderContext
   migration tail (OffscreenViewScope swaps ::View/MainSurf/FOV globals —
   cannot run concurrently with anything). When renderFrame takes a
   RenderContext (RENDER_CONTEXT_PLAN Slice-3 tail), the RTT can go async
   with 1-frame-stale output, OR fan its panels Slice-6-style. That migration
   has value beyond perf; do the RTT as its first payoff.
2. The in-render lighting-orchestrator serial gap (~1.75 ms) — profile what
   it actually is (ViewLightsSoA build? mat32 clear? dispatch?) before
   assuming it parallelizes.
3. Tick-Xfrm 0.44 — parallel Transform needs per-worker face lists + a merge
   (or atomic FList cursor); small prize, medium risk.

Measured floors this session (idle, ts=491, quarter, full config):
pre-session 43.4-44.4 → with P1 ~43.4-43.9 (Tick-Light −0.34).

## 2026-07-03 (later) — the orchestrator gap profiled: it was ENQUEUE overhead

New TailProf marks inside Render_DeferredLighting decomposed the ~1.75 ms
unaccounted serial: light-list 0.011 ms (the linear shadow-map scans are
noise), mirror-grid 0.524 ms (full-res mirrorMask scan — still serial,
parallelizable follow-up), and the rest was the DISPATCH LOOPS themselves:
96 ThreadPool::enqueue x ~12 us (queue mutex + notify_one while workers park,
plus the woken workers contending the same mutex to pop) = ~1.2 ms serial per
wave, x2 lighting waves + the cones wave.

Fix (348216c): work-stealing chunk dispatch — enqueue only W tasks, each
pulling tiles off a by-value shared_ptr atomic cursor (straggler-safe: a
worker can evaluate the loop condition after the drain returns; the first cut
captured the stack cursor by reference and crashed intermittently). Per-tile
tileDone releases unchanged -> byte-identical. w1-enqueue 1.20 -> 0.09 ms;
ts=491 quarter floor 43.4 -> 40.5 ms min. hdrDispatchRows converted too —
neutral (post-pass workers rarely park so notify_one was cheap) but
structurally better.

Remaining serial leads, updated: mirror-grid 0.524 (parallelize the scan),
RTT 1.68 (blocked on context migration), Tick-Xfrm 0.44. The gate now runs
HEADLESS (SDL_VIDEODRIVER=dummy, rebaselined) — it was popping a window on
the desktop every run.

## 2026-07-03 (later still) — full enqueue-loop audit + P-core scheduling answer

Every remaining task-per-tile enqueue loop converted to the new Threads.h
`dispatchIndexed` (shadow bake Phase A/B, SSAO x5, EdgeAA, skybox, halos, fog
composite, gbuffer fill, Hdr tile helper). All byte-neutral (gate x3, 5-frame
greets sequence zero-pixel, ASan clean). Floor stayed ~40.4 — the extra loops'
enqueues were largely hidden behind already-busy workers (notify_one is only
expensive when workers are PARKED, i.e. at post-barrier wave starts — which is
why lighting/cones were the ones worth ms). Kept for robustness: the helper
encodes both lifetime traps (temporary fn dies before the caller's drain →
copy fn per task; thread_local job vectors re-resolve on the worker → snapshot
.data()) that each cost a crash-bisect during development.

**P-core-only scheduling: measured NO — it loses.** 8P+4E box, workers already
QOS_USER_INTERACTIVE: pool=8 (P-only) = 43.0-43.2 ms min vs default 10 = 40.4,
all 12 = 40.4. E-cores are net-positive; work-stealing chunks self-balance
around their slowness. Pool sizing stays min(16, cores-2).

Bonus ASan find while validating: GREETS SceneCorrections() read past its
9-entry ObjStationary[] stack array for every mesh beyond the 9th — nonzero
stack garbage would randomly mark meshes stationary (a latent nondeterminism
source). Bounds-guarded, byte-neutral in release.

## 2026-07-03 — mirror double-shading MEASURED (greets teleporter, coverage sweep)

Full-screen mirror (cam 0,2.5,-2.5 facing teleporter, t=700, quarter, full
config): frame min 20.9 (mirror off) -> 39.0 (on) = +18 ms. Repeatable x2
interleaved. Decomposition of the delta:

| pass | off | on | delta |
|---|--:|--:|--:|
| xpar-peel (the GLASS wall is a transparent face!) | 0.0 | 5.2 | +5.2 |
| lighting-w1 (clone surfaces + clone omnis in tiles) | 5.0 | 9.6 | +4.6 |
| lighting-w2 (fill over mirror pixels) | 2.1 | 3.8 | +1.7 |
| cones (clone beams in reflection) | 0.5 | 2.3 | +1.8 |
| RTT + StampMasks + mirror-grid + Probe | 0.0 | 2.0 | +2.0 |
| (remainder: clone raster in gbuffer + xform) | | | ~+2.7 |

The single biggest line is NOT the reflected-surface kernel — it's the
half-silvered GLASS overlay going through the transparent peel, which
re-lights every covered pixel at full rate. Levers, by size:
1. Cheap-glass path for the mirror wall in the peel (near-uniform tint —
   doesn't need the full per-pixel light loop), or quarter/checker rate for
   the transparent kernel. ~5 ms at full coverage.
2. Reduced-rate (quarter/checker) shading for clone-layer pixels. ~6 ms.
3. Cone rate/cap inside reflections. ~2 ms.
Costs scale ~linearly with mirror screen coverage; at typical greets framing
(mirror ~15-30% of screen) the total is the ~6 ms measured in the t=1130
cluster investigation.

## 2026-07-03 — the t=1130-1162 slow cluster MEASURED (window bench)

`--bench=scene@scene=greets,ts=1130,tend=1162,iters=120` full user config
(quarter + ssao_temporal), mirror on/off interleaved x2:

| | min | p50 |
|---|--:|--:|
| mirror on | 41.0-41.4 | 50.7 |
| mirror off | 37.6-38.7 | 43.5 |
| mirror total | ~3.3 | ~7 |

In-window TailProf (mirror on): lighting w1+w2 = 14.7 wall (vs ~11 at
ts=491); **cones = 6.55 wall / 72.8 ms POOL BUSY** (3x the ts=491 cone
load — the teleport-event beams + bounce spots + clone beams stack);
xpar-peel total only 1.53 (glass composite 0.61, clones 0.03);
StampMasks+grid 0.8.

Verdicts for the cluster:
- The glass cheap-path (option b) is worth only ~0.6 ms here — it's a
  point-blank-pose optimization, not a cluster fix.
- The two levers that pay at BOTH ts=491 and the cluster:
  (1) cones — biggest single block, 72.8 ms of pool work in-window;
  (2) clone lights in the kernel tile lists (option c) — footprint cull
  already exists (computeMirrorPresenceGrid) but inside footprint tiles
  every pixel still iterates the clone lights.
User approved: "let's try to attack the cones/clones".

## 2026-07-04 — clone-cone footprint cull SHIPPED (5a58269); clone-kernel lever measured DEAD

FDS_CONE_ATTR revealed 40 of 50 cone spots were mirror clones, ~75% of the
(tile × spot) volume; the cone/halo binning had no mirror-footprint cull
(the kernel's list builder did). Fix: reuse computeMirrorPresenceGrid bits
via ctx (copied BY VALUE — offscreen bakes recompute the static grid
mid-frame). Exactly conservative (per-pixel gate is mmask==id): 9-pose
warm sequence byte-identical, gate ALL PASS.

Window t=1130-1162: cones wall 6.4-7.5 → 3.6-4.1 ms (busy 71-83 → 39-43),
frame p50 −4 ms, matching the FDS_CONE_SKIP_CLONE ablation floor with the
feature intact. ts=491: cones 2.5 → 1.2, frame ~−1.3 ms.

**Clone lights in the KERNEL are already contained** — post-fix window
measurement: numLights 33 → 117 with mirror on, but avg lights/tile 10.5
vs 10.9 and w1 10.0 vs 10.1 ms (the pre-fix "10.8 vs ~8" gap was load
noise). Don't build a clone-light cap. Remaining mirror delta in the
window (~+2.5 min / +4 p50 ms) is machinery: xpar peel 1.3-1.5, RTT 0.93,
StampMasks+grid 0.8, clone raster/transform — no single big lever left;
next-largest single blocks frame-wide are unchanged (kernel, SSAO, post).
