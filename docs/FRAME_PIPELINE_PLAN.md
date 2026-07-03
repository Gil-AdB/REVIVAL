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

**P1 — parallel vertex Lighting (`--vertex_light_parallel`, default ON).**
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
