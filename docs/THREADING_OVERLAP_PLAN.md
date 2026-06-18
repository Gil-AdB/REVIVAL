# Stage A — Shadow-bake ∥ gbuffer-fill overlap (detailed plan)

Status: **NOT IMPLEMENTED** (the safe freebie `Hdr_ActivateNoFog` threading already
shipped as `84f189e`). This is the big overlap. It is the **highest-risk change in the
codebase** — it lives in the exact shadow-bake code that hosted the TSan-invisible
torn-read that cost ~5 sessions (see memory `project_shadow_tile_flicker_hunt` +
`reference_tsan_atomic_blindspot`). Do it unhurried, gated, and validate with the
concurrent-bench STEADY-count, NOT just TSan / the byte-gate.

## Goal

Today, per greets frame: `Render_DeferredShadowMaps` (the dynamic shadow re-bake) runs
**serially before** the gbuffer fill, both on the tick thread, both draining the shared
`renderns::tileDone` semaphore. They are data-disjoint (shadow bake writes per-light
VertexScratch clones + shadow maps; gbuffer writes the g-buffer planes), so they CAN run
concurrently. Overlapping them reclaims the shadow-bake wall-time (the ~27% worker idle in
RNDR is the barrier tail; this fills part of it).

## The three shared primitives (all must be isolated)

The shadow bake (`FDS/RENDER/Shadows.cpp`, `Render_DeferredShadowMaps`) shares these with
the gbuffer fill (`renderns` namespace, defined `FDS/RENDER/RENDER.CPP:247-266`):

1. **`renderns::tileDone`** (counting_semaphore) — shadow bake release/acquire at
   Shadows.cpp **434, 438** (Phase A transform) and **639, 645** (Phase B raster).
   → Give the shadow bake its **own** `renderns::shadowDone` semaphore. Pool workers
   release the right semaphore per task type (the task lambda calls the release), so a
   worker can service a gbuffer tile (→`tileDone`) and a shadow tile (→`shadowDone`)
   interchangeably. No collision.

2. **`renderns::tileCounter`** (atomic<int>) — shadow bake **resets it to 0** at
   Shadows.cpp **270-271** and **462-463** (under `tileCounterMutex`). The Phase A/B task
   bodies **pre-assign tiles** (capture `tx`/`ty`/`x1f`…) and do **NOT** `fetch_add` on
   `tileCounter` — confirmed by grep, no work-stealing in the shadow tasks. So the reset is
   **vestigial for the shadow bake**. BUT concurrent with the gbuffer it's a write-race if
   anything on the tick thread reads `tileCounter` during the bake.
   → Investigation result: `tileCounter` is reset (=0) by the cone/lighting passes
   (DeferredVolumetric.cpp:1569/2329/2445/2614, DeferredSurfaceKernel.cpp:3278/3303) which
   run **after** the join, and the gbuffer-fill tile dispatch (RENDER.CPP ~414-432) +
   `RenderInnerMekalele` do **not** appear to read it (RenderInner.cpp only `extern`s it).
   → **Safest fix:** when running async, the shadow bake must NOT touch `tileCounter`.
   Gate the two resets (270/462) on `!async` (serial keeps them; async skips them — the
   shadow tasks don't use the counter, and the post-join lighting resets it anyway). Do
   **not** remove them outright — keep serial behavior byte-identical.

3. **The join seam** — lighting samples the shadow maps, so the bake MUST complete before
   `Render_DeferredLighting`. That call is **`FDS/RENDER/RENDER.CPP:458`** (inside
   `renderFrame`, after the gbuffer fill, after `Hdr_BeginFrame` at 451). The shard/RTT
   recursive lighting passes also call lighting — they must NOT join the main bake.

## Implementation (gated behind `--shadow-gbuffer-overlap`, default OFF)

Gating makes it byte-identical when off (gate stays green) and lets the user A/B + run the
full STEADY-count before it's ever defaulted on. New flag in `FDS/Base/FeatureFlags.def`:
`FDS_FLAG_BOOL(shadow_gbuffer_overlap, "shadow-gbuffer-overlap", 0, ...)`.

### 1. `renderns::shadowDone` semaphore
- `RENDER.CPP:259` area — add `std::counting_semaphore<INT_MAX> shadowDone{0};` next to
  `tileDone`.
- `Shadows.cpp` — add `extern std::counting_semaphore<INT_MAX> shadowDone;` to the
  `renderns` extern block (near where `tileDone` is extern'd, ~line 50).
- Replace the 4 `renderns::tileDone` → `renderns::shadowDone` at 434/438/639/645.
- This change alone is **behavior-preserving even serially** (the bake just drains its own
  semaphore). Land + gate-check this first as an isolated commit.

### 2. `tileCounter` reset guard
- `Shadows.cpp:270-271` and `462-463` — wrap the reset in
  `if (!fds::FeatureFlags::shadow_gbuffer_overlap()) { ...reset... }`. Serial path
  unchanged; async path leaves `tileCounter` to its post-join owner.

### 3. Async spawn (greets only)
- `DEMO/GREETS.CPP` — find the serial `Render_DeferredShadowMaps(...)` call in the greets
  render path (the per-frame dynamic re-bake, NOT the static init bake which is already a
  background thread — see `project_greets_scene_defaults` / `LightmapBake_Static`). The
  per-frame dynamic bake is the one inside `Render()`/the greets frame; locate via
  `grep -n Render_DeferredShadowMaps DEMO/*.CPP FDS/RENDER/*.cpp`.
- When `shadow_gbuffer_overlap()`: spawn it on a thread (store the handle in a file-scope
  `std::thread g_shadowBakeThread` + a `std::atomic<bool> g_shadowBakePending`), BEFORE the
  gbuffer fill kicks off (i.e. before `Render(ForceDeferred)` or at the point the serial
  call sat). Else call it inline (current behavior).
- NOTE: confirm where `Render_DeferredShadowMaps` is actually invoked for greets — it may
  be inside `Render()` (RENDER.CPP), not GREETS.CPP. If so, the spawn/branch goes there,
  gated on `shadow_gbuffer_overlap()` so only greets (which sets the flag) overlaps.

### 4. Join before lighting
- `RENDER.CPP:458`, immediately before `Render_DeferredLighting(dctx)`:
  `if (g_shadowBakePending.exchange(false)) { g_shadowBakeThread.join(); }`
- Guard so the recursive shard/RTT lighting passes (which also reach lighting) do NOT
  attempt the join — the pending flag is set once per main frame and cleared by the first
  (main) join, so recursive passes see `false`. Verify the recursive passes run AFTER the
  main join (they do — RTT/shards composite into the main frame).

## Deadlock analysis (must hold)
- Shadow thread (non-pool) enqueues tasks → drains `shadowDone`. Tick thread (non-pool)
  enqueues gbuffer tiles → drains `tileDone`. Pool workers process both queues, releasing
  the matching semaphore per task. No pool worker ever blocks on a semaphore (only the two
  orchestrator threads do). → no deadlock.
- Phase A→B barrier inside the bake is internal to the shadow thread (it drains shadowDone
  between phases) — unaffected by the gbuffer.

## Data-isolation audit (must hold)
- Shadow bake writes: per-light `VertexScratch` clones (`scratchPtr`), the cube/2D shadow
  maps (`sm.depth`), thread_local mode flags (`g_inShadowPass`, `g_currentShadowMap` —
  Transform.cpp:89/516/528/569). `BSphereScreenPos` is written only `!_inShadowPass`
  (Transform.cpp:980) — so the shadow pass does NOT touch it; the concurrent gbuffer/main
  transform owns it. ✓
- Gbuffer fill writes: g-buffer planes, `g_mainFaces` transform output. Disjoint from the
  shadow scratch. ✓
- `StampMirrorMasks` + `gg->Render()` (no-op) run on the tick thread between spawn and the
  gbuffer fill — confirm neither mutates scene geometry the shadow bake reads (they read
  the gbuffer / mirror plane, not per-light scratch). ✓ expected, but the concurrent-bench
  is what proves it.

## Validation (REQUIRED — in order)
1. **Build clean.**
2. **Byte-gate** `tools/render_gate.sh` with flag OFF → must match (mirrortest/conetest/
   halotest md5 unchanged; conetest exercises `--shadows` so it covers the semaphore swap).
3. **`FDS_THREADS=1`** greets snapshot, flag ON vs OFF → byte-identical (serializes the
   pool; proves the overlap doesn't change results, only timing).
4. **`FDS_THREADS=N`** greets snapshot, flag ON vs OFF → should match (or differ only in
   the known nondeterministic-but-stable way). A *consistent* diff here = a real race.
5. **12-concurrent-bench STEADY-event count under load**, flag ON, greets, probe f>4 —
   the TSan-blind torn-read detector. Count must stay 0 across many runs. THIS is the gate
   that matters; TSan + the byte-gate are blind to this race class.
6. Measure the greets RNDR gain (flag ON vs OFF, idle machine, user go-ahead) — confirm it
   actually reclaims wall-time before defaulting on.

## Only default ON after step 5 is clean across many runs. Until then: ship gated OFF.
