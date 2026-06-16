# RenderContext — thread-safe render state (migration plan)

## Why

The engine's render state lives in file-scope globals: `MainSurf`/`VPage`/
`ZPage16`/`XRes`/`YRes`/`VESA_BPSL`, the deferred G-buffer set (`g_gbuffer`,
`g_gbufferTransparent[Back]`, `g_xparZ[Back]`), the view/projection
(`View`/`FOVX`/`FOVY`/`CntrE*`/`Cntr*`/`AspectRatio`), the depth encoding
(`C_NZP`/`C_FZP`/`g_zscale`), the face list (`FList`/`SList`/`CAll`), and
`CurScene`. Any pass that renders to a side target swaps these globals and
restores them (`OffscreenViewScope` for the mirror RTT; the CITY cube bake
hand-rolls the same dance). That makes offscreen passes **mutually
exclusive** — they can't run concurrently, and they can't pick a render
path independent of the global default.

Two concrete things this blocks today:

1. **Parallelism.** The mirror-shard reflection pass runs N=238 small
   offscreen renders serially (~42 ms). Each mutates the globals, so they
   can't be fanned across cores. A thread-safe context lets each worker own
   its render state → run them concurrently (ceiling ~N_core×).
2. **Deferred offscreen bakes.** `g_gbuffer` is one global sized to the
   engine framebuffer, so any offscreen render falls back to forward (the
   size check in `renderFrame`). That's why the shard / mirror-RTT bakes are
   forward-only. A per-context G-buffer (sized to the bake target) makes a
   *deferred* offscreen bake possible — higher quality (shadowed, per-pixel),
   matching the main view exactly. Useful for the shards AND any future
   offscreen deferred need.

This refactor is **not** required for correctness anymore (the forward/
deferred color divergence was a separate R/B-swap bug, fixed in
`TheOtherBarry`'s `colorize` — see git log). It is purely an
architecture/perf/optionality play.

**Update after Slice 2 measurement:** deferred offscreen bake costs ~4× the
forward inline bake at 64² (fixed dispatch + lighting-pass overhead × N),
for marginal quality — so deferred-offscreen is *not* a motivation. The
remaining justification is **thread-safety + inter-render parallelism**,
which is bigger than the shards alone (decision: proceed).

### Two threading models (the real point)

- **One big render** (main view): *intra*-render tiling — all cores on one
  frame. This is what the engine does today and should keep doing.
- **Many small renders** (shadow maps over N lights, mirror RTT over N
  panels, shard reflections over N=238): *inter*-render parallelism — each
  render runs **whole, single-threaded** on a pool thread, and N of them fan
  across the pool. Two reasons: (a) per-render threadpool dispatch is the
  dominant cost at small sizes (Slice-2 showed it), so tiling each one is
  pure overhead; (b) a pool thread doing a *complete* render never
  re-submits to the pool → no nesting/deadlock.

All three "many small" workloads are throttled today by global render state:

- **Shadow bake** (`Shadows.cpp`) parallelizes per-light *Transform* only;
  the raster is serial because the framebuffer/G-buffer/FList are global
  (its own comment: "if Transform >> Raster, cross-light parallelism is
  moot"). Per-context state lets the raster go cross-light too.
- **Mirror RTT** (`GreetsMirror.cpp:2457`) is a fully serial slot loop.
- **Shard reflections** — serial loop + global swap.

So the migration's payoff is **inter-render parallelism for every offscreen
workload**, not just the shards. That's why it's worth doing.

## Current state — what's already done

The hard part of the data modelling is already in place (the SoA / FrameState
work laid it):

- **`fds::CameraContext`** (`Base/CameraContext.h`) — `view`, `fovX/Y`,
  `cntrX/Y`, `cntrEX/Y`, `nearZ/farZ` (+ reciprocals), `zScale[256]`. The
  fillers and `FrustumClipper` take `const CameraContext&`.
- **`fds::FaceListContext`** (`Base/FaceListContext.h`) — `fList`, `sList`,
  `cAll`/`cPolys`/`cOmnies`/`cPcls`. `Transform_Objects(sc, cam, faces)`
  writes into a *passed* one.
- **`fds::RenderTarget`** (`Base/RenderTarget.h`) — `vpage`,
  `bytesPerScanline`, `zpage16`, `xres/yres`, + the 5 G-buffer pointers.
  Per-pixel rasterizers take `const RenderTarget&`.
- The legacy global names are **references/aliases** into
  `fds::g_mainCamera` / `fds::g_mainFaces` (`FrameState.cpp`), so unmigrated
  code still compiles and reads the same memory.

**The gap:** the globals are still the *source of truth*. `Render()` builds
its `RenderTarget` via `MainRenderTargetFromGlobals()` every frame, and the
orchestration (`renderFrame`, `RenderInner*`, the deferred kernel + its
volumetric/fog passes, `Transform_Objects`' projection reads) still read the
globals directly in their hot loops. So there is exactly one logical context,
backed by globals.

## The target

A `RenderContext` that bundles the three existing structs + scene + the
target's owned buffers, and is **the** source of truth:

```
struct RenderContext {
    RenderTarget    target;     // surface + gbuffer (already exists)
    CameraContext   camera;     // view + projection (already exists)
    FaceListContext faces;      // FList/SList/CAll (already exists)
    Scene          *scene;      // replaces CurScene
    // owned buffers for non-primary contexts (offscreen):
    //   color, z16, gbuffer set — allocated to target dims.
};
```

- The **primary context** (`g_primaryContext`) is the engine framebuffer.
  The legacy globals become a thin view onto it (keep the aliases; point
  `MainRenderTargetFromGlobals` at `g_primaryContext.target`). Net behaviour
  unchanged → byte-identical.
- **Offscreen contexts** (shard bake, mirror RTT, cube bake) own their
  buffers and are passed explicitly. No global swap, no mutex.
- **Threading is the caller's choice**, not baked into the engine: the main
  frame uses the primary context and tiles across the pool (unchanged); the
  shard pass fans a pool of small contexts across threads; shadow/RTT use
  their own. The engine no longer dictates "one render at a time."

Note the **per-pass thread_local state** (`g_clipperTileRect`,
`g_currentShadowMap`, `g_inShadowPass`) already exists and is correct for
*intra*-pass tiling. For *inter*-pass concurrency it's also fine — each OS
thread has its own — as long as a single pass doesn't span threads with
mismatched expectations. The migration must keep these per-thread.

## Migration — incremental, byte-gated

Alias-and-migrate. At every step the engine compiles and the **primary
(main-frame) path stays byte-identical**; verify with the established
3-scene / 3-pose pixel gate (city t=280 fog, greets t=700 mirror+rtt+shadows,
mirrortest 8-pose) after each slice. Offscreen-rendered pixels (reflective
faces, bakes) are *expected* to change only where the deferred path replaces
forward — gate those separately by eyeball.

**Slice 1 — `RenderContext` skeleton + primary instance (no behaviour change).**
Introduce the struct; create `g_primaryContext`; make
`MainRenderTargetFromGlobals()` return `g_primaryContext.target`; leave all
globals as-is. Pure plumbing. Byte-identical by construction.

**Slice 2 — per-context G-buffer (deferred offscreen bake). DONE (as an
interim global-swap) + MEASURED. Verdict: not worth it as default.**
Implemented an opt-in deferred shard bake (`FDS_SHARD_DEFERRED`): allocate a
texRes²-sized G-buffer set, swap it into the `g_gbuffer` globals around a
`Render(ForceDeferred, skipVolumetric)`, restore. Measured at 64², N=238:

    forward inline bake   ~41 ms
    deferred bake        ~160 ms   (~4×)

The full-frame 8× ratio did shrink (the per-pixel lighting cost collapsed at
64²), but **fixed per-call overhead dominates** — threadpool dispatch (which
`RenderForwardRegionInline` avoids) + the deferred lighting pass setup
(tile-light-list build, mat32 clear), ×238. Quality gain over the
R/B-fixed forward bake is marginal (slightly shadowed). **Conclusion: keep
the forward inline bake as default; the deferred path stays opt-in** (for
low-shard-count / paused scenarios, or future use). This also removes
deferred-offscreen as a motivation for the rest of the migration — see below.

**Slice 3 — thread the context through the orchestration. (The big one.)**
`renderFrame`, `RenderInner*`, `Render_DeferredLighting` + volumetric/fog
passes, and `Transform_Objects`' projection reads take `const RenderContext&`
(or the sub-structs) instead of reading globals. The hot inner loops
(`Mekalele`, `TheOtherBarry`) already take `rt`/`cam`; `Transform_Objects`
already takes `(sc, cam, faces)` and reads `cam.fovX` etc. The remaining work
is the *passes that call them* + the per-pixel deferred kernel's direct global
reads.

Audited scope (`FOVX/FOVY/CntrE*/Cntr*/XRes/YRes/VPage/ZPage16/VESA_BPSL/
CurScene/g_gbuffer/g_xparZ/FList/SList/CAll/C_NZP/C_FZP/g_zscale` reads):

    DeferredFastFog.cpp        106
    DeferredVolumetric.cpp      91
    RENDER.CPP                  90
    DeferredSurfaceKernel.cpp   80
    RenderInner.cpp             23
    DeferredLightLists.cpp      12
    ~400 sites total (mostly repeats of XRes / FOVX / CntrEX / CurScene /
    g_gbuffer — mechanical, but each must not perturb the main path's bytes).

**Tactic:** migrate **one function at a time**, threading a
`const RenderContext&` (or the sub-struct it needs), replacing that
function's global reads with ctx fields, then run the 3-scene/3-pose byte
gate before moving on. Order: leaf-most first (DeferredLightLists,
RenderInner) → kernel (DeferredSurfaceKernel) → volumetric/fog → renderFrame
last (it builds the sub-contexts; once it takes a `RenderContext`, the
offscreen callers stop swapping globals). This is a dedicated multi-session
campaign — start it fresh, not as a tail to other work; it is well-suited to
careful incremental execution but NOT to blind delegation (hot perf code,
byte-exact gate).

**Slice 4 — thread `CurScene` → `ctx.scene`.** Mechanical; many call sites.

**Slice 5 — delete the globals.** Once nothing reads them, remove the
aliases. The `OffscreenViewScope` dance disappears (contexts don't swap
globals). CITY cube bake + mirror RTT adopt contexts.

**Slice 6 — parallelize the shard pass.** Fan the per-shard contexts across
the pool (atlas cells are disjoint → no write contention). Re-measure.

## Class-based end-state (the actual goal — supersedes per-kernel aliasing)

LSP (`findReferences` on `g_deferredCtx`) showed the real picture: the
volumetric/fastfog passes **already read the file-scope `g_deferredCtx`
singleton pervasively** (lights, matTable, Sc, invFOVX, and now addressing).
So further "alias the global onto the ctx" steps in those files are
**low-value** — they remove global *names* but the **singleton is what blocks
parallelism**. (Also: LSP lists `DeferredLighting.cpp` references — that file
was renamed to `DeferredSurfaceKernel.cpp`; the hits are a stale clangd index,
ignore them.)

The clean end-state, per the "use classes" direction:

- **`fds::RenderPipeline` owns a `RenderContext`** (it already owns
  `renderFrame`). The per-frame state — the `DeferredLightingCtx` contents +
  target/gbuffer + face list — lives in the instance, not file scope.
- **`renderFrame` builds/receives the `RenderContext`** and threads
  `const RenderContext&` (or `DeferredLightingCtx&`) to every pass:
  `Render_DeferredLighting(ctx)`, the volumetric/fog orchestrators(ctx) →
  their `_Tile(ctx)`. The kernels already take ctx by param; the work is the
  **orchestrators + `renderFrame`**.
- **Delete the `g_deferredCtx` singleton** — the stack-local (or per-instance)
  ctx is the only source.
- **Parallel offscreen renders** = separate `RenderPipeline` instances (or
  separate `RenderContext`s), each on its own thread. No global swap.

### Singleton-deletion touch points (enumerated — the atomic final flip)

All hot **tile kernels** now take `const DeferredLightingCtx&` by param
(surface: 4 kernels; volumetric: cone + halo tiles). Remaining readers of the
file-scope `g_deferredCtx` are **orchestrators + one cross-file site**:

- `DeferredSurfaceKernel.cpp`: `g_deferredCtx` def (95); `Render_DeferredLighting`
  builds it (3003-3004); **`RenderXparClumpInStrip` copies it (1745)**.
- `DeferredVolumetric.cpp`: cones orchestrator (1431), halos orchestrator (2241)
  — read for lights/numLights (tiles already get ctx).
- `DeferredFastFog.cpp`: `Render_DeferredFastFog` (1787), `…SkyPaint` (1992),
  `Render_DeferredVolumetric` (3074) — build `FastFogParams` from it.
- `DeferredCommon.h`: extern decl (235).
- **`FILLERS.CPP:1866`** calls `RenderXparClumpInStrip` from the unified-TBR
  transparent path — so threading the strip ctx reaches into FILLERS' TBR
  code. This is the coupling that makes the deletion an atomic multi-file
  change (can't gate mid-way), and why it's its own focused unit.

Flip: `renderFrame` declares a stack `DeferredLightingCtx ctx`,
`Render_DeferredLighting(ctx)` fills it, passes `ctx` to the volumetric/fastfog
orchestrators (skybox + fogpass don't read it); thread ctx to
`RenderXparClumpInStrip` via its FILLERS caller; delete the def + extern.
Build + `render_gate.sh`. Then Slice 6: per-instance pipelines for parallelism.

### Revised remaining steps (do these; skip more singleton-aliasing)

1. Give the volumetric/fastfog **orchestrators** a `DeferredLightingCtx&`
   param; thread it to their `_Tile` functions (replace the `g_deferredCtx`
   reads with the param). LSP `findReferences` enumerates the sites.
2. `Render_DeferredLighting` / the orchestrators are **called from
   `renderFrame`** — make `renderFrame` own a stack-local `DeferredLightingCtx`
   (built where `g_deferredCtx` is built today) and pass it to all of them.
3. Delete `g_deferredCtx`.
4. Thread the remaining `renderFrame` globals (XRes/VPage/FList/CAll/CurScene)
   into the `RenderContext`; the tile-dispatch lambdas capture it.
5. Slice 6: parallelize the offscreen workloads via per-instance pipelines.

Each step still gates with `tools/render_gate.sh`. This is the backbone and
the highest-risk change — do it as a focused effort, not tacked onto other
work.

## Forward path (the parallelism-critical path) — status + Slice 6 recipe

Confirmed by call-graph tracing: the parallel offscreen workloads
(shard/shadow/RTT bakes) run **forward** (`RenderForwardRegionInline` →
`RenderInner` → `TheOtherBarry` + `Transform_Objects`) — they never touch
`g_deferredCtx`. So the forward path, not the deferred kernel, is what
parallelism needs.

Done: `RenderInner` / `RenderForwardRegionInline` take a per-pass
`fds::RenderContext` (target + camera + faces + scene); renderFrame passes the
primary context; the shard bake passes `primaryRenderContext()` over its
OffscreenViewScope-swapped globals (5adda03). So a worker can now forward-
raster *its own* face list into *its own* surface.

**Slice 6 — parallelize the shard pass.** Each worker needs:
1. Own `FaceListContext` — done (`ctx.faces`); `Transform_Objects` already
   writes a passed one.
2. Own **`VertexScratch`** — THE catch. `Transform_Objects` writes transformed
   verts into per-mesh `VertexFrame` storage shared across the scene;
   concurrent transforms clobber each other. Mirror the shadow bake
   (`Shadows.cpp`: `perLightScratch[lightIdx]`, a `fds::VertexScratch` per
   worker, stamped so `F->frame` points at the worker's scratch not the shared
   mesh frame). This is the real work of Slice 6.
3. Own surface (pool of N 64² targets, not the single `reflSurf_`) + own camera.
4. Fan the shards across the pool: each worker builds its camera +
   `Transform_Objects(sc, ctx.camera, ctx.faces)` into its scratch +
   `RenderForwardRegionInline(ctx)` + blit its atlas cell (cells are disjoint).

**Slice 6 is CONCURRENCY code — `render_gate.sh` does NOT validate it.** The
gate runs serially; it proves byte-identity, not thread-safety. Validate with
TSan + the concurrent-bench methodology (see the shadow-flicker memory: torn
reads across atomics are TSan-invisible and cost ~5 sessions). Audit every
remaining shared mutable touched on the forward path under concurrency
(per-mesh `VertexFrame`, `MatShadowCache`, any `thread_local` assumed
single-pass, the clipper tile rect). Do this fresh, with TSan, NOT as a tail
to other work — the gate's green light is necessary but nowhere near
sufficient here.

## Risks

- **Inner-loop global reads** (`FOVX`/`CntrEX` in Transform & the deferred
  kernel) are the fiddly part — each must become a ctx field without
  perturbing the main path's byte output. The aliasing keeps every step
  green; the pixel gate catches regressions.
- **Scope creep into the whole renderer.** Keep slices small; never let a
  slice touch both the main path's correctness and a new feature at once.
- **thread_local assumptions.** Verify no pass implicitly relies on a global
  set by a *different* thread once contexts run concurrently (Slice 6).

## Verification

**Byte gate (per Slice-3 step): `tools/render_gate.sh`.** One command, three
deterministic deferred scenes (stable run-to-run AND threaded==serial), each
compared to a committed-state baseline md5. greets is NOT usable (timing-
dependent background lightmap bake); city unverified — don't add without
re-checking determinism.

    ./tools/render_gate.sh            # PASS/FAIL per gate, nonzero exit on fail
    ./tools/render_gate.sh --update   # reprint md5s to re-baseline after an
                                      # INTENDED change

Coverage:
  - mirrortest (8-pose)  — deferred surface kernel + mirror clone + RTT
  - conetest  (12-pose)  — DeferredVolumetric cones + DeferredFastFog (fog on)
  - halotest  (7-pose)   — DeferredVolumetric omni halos

So all of DeferredSurfaceKernel / DeferredVolumetric / DeferredFastFog are now
gate-covered — the fog-gate gap is closed. (City env path still uncovered; not
on the Slice-3 critical path.)

- Offscreen quality: eyeball greets shatter + mirror RTT after slices 2, 6.
- Perf: `FDS_SHARD_REFL_PROF` (per-pass ms), `FDS_RNDR_BENCH` (forward vs
  deferred per-frame), `FDS_SHARD_REFL_LOOP` (extend runtime for sampling).
  Kept gated in the tree for this work.

## Progress log (current)

**Both legs' render state is now context-owned (gate-verified).**
- Forward (parallelism-critical): `RenderInner`/`RenderForwardRegionInline`
  take a per-pass `fds::RenderContext` (5adda03). renderFrame builds the
  primary context; the shard bake passes its own.
- Deferred: `renderFrame` owns a stack `DeferredLightingCtx`, fills it via
  `Render_DeferredLighting(ctx)`, threads it to all volumetric/fog
  orchestrators; tile kernels take it by param (d4e2e49, 39b6110, e96b799,
  50ffe20). `g_deferredCtx` survives only as a *published copy* renderFrame
  writes, read by two un-threaded sites: `RenderXparClumpInStrip` (FILLERS TBR
  strip path) and the fog tile.

**Gate-coverable de-globalization is essentially complete.** What remains:
1. **Delete the published `g_deferredCtx`** — thread the ctx through
   `TBR_Render` → `RenderXparClumpInStrip` (FILLERS.CPP) + the fog tile. Small
   + mechanical, but the TBR strip path is NOT covered by render_gate.sh
   (mirrortest/city/greets fall through to the legacy peel; need a TBR scene
   or manual check). Low priority — it's a published copy, not a divergence.
2. **Slice 6 — parallelize** (per-worker `VertexScratch`/surface/camera).
   CONCURRENCY code: render_gate.sh canNOT validate it. Needs TSan + the
   concurrent-bench methodology (shadow-flicker lesson). Do fresh.

## Progress log (history)

- Slice 1 — DONE (`primaryRenderContext()` scaffolding).
- Slice 2 — DONE + measured; deferred bake kept opt-in (forward default).
- Slice 3 — IN PROGRESS. Done:
  - `DeferredLightLists.cpp` (821f93a, full).
  - `RenderInner.cpp` mask gate → rt (41fa4ab; FList/CAll/CurScene remain —
    tied to the tile-dispatch signature, migrate with renderFrame).
  - `tools/render_gate.sh` (c9bf056) — mirrortest + conetest(+fog) + halotest,
    all deterministic, all deferred TUs covered.
  - `DeferredSurfaceKernel.cpp` (d4e2e49) — the 4 ctx-taking tile kernels
    (Tile, TransparentTile, OuterVec, TileFill) alias addressing from ctx
    (extended DeferredLightingCtx with xres/yres/vpage/zpage16/cntrE*). Still
    global: RenderXparClumpInStrip, the xpar-layer pointer select, orchestrator.
  - `DeferredVolumetric.cpp` fog tile (e96b799) — ctx gained fovX/fovY/zscale;
    Render_DeferredFogPass_Tile aliases from g_deferredCtx (safe: filled before
    volumetric passes run).

  **RESUME HERE — `DeferredVolumetric.cpp` cones + halos tiles** (large
  kernels — check each function's exact extent before aliasing), then their
  orchestrators; `DeferredFastFog.cpp` (106); then the backbone:

  Original audit (`DeferredSurfaceKernel.cpp` 80 etc) below for reference.
  NOTE: the alias-from-g_deferredCtx steps remove *direct* global reads but
  g_deferredCtx is still a file-scope singleton — true per-pass parallelism
  needs renderFrame to build + thread a per-pass RenderContext (Slice 3 tail).

  Original target list:
  Tally: XRes×29, YRes×15, VPage×12, ZPage16×10, CurScene×7, g_gbuffer×5,
  CntrEX×5, g_zscale×4, g_xparZ[Back]×8, CntrEY×4, FOVX/Y×2. The kernel
  functions already take `const DeferredLightingCtx& ctx` (carries gb,
  invFOVX/Y, invZScale, Sc). Migration: extend `DeferredLightingCtx` with the
  addressing fields (`int xres,yres,bpsl; byte* vpage; word* zpage16; float
  cntrEX,cntrEY;` + the transparent gbuffer/xparZ pointers), populate once in
  `Render_DeferredLighting` from the globals, then add a small alias block at
  each kernel function's entry (`const int XRes = ctx.xres;` …) so the bodies
  are untouched (same trick as buildTileLightLists). Gate: mirrortest.

  Then: `DeferredVolumetric.cpp` (91) / `DeferredFastFog.cpp` (106) — now
  gate-covered (conetest cones+fog, halotest halos). Finally
  `RENDER.CPP` renderFrame (90, last): build `ctx = primaryRenderContext()`
  at top, thread it into the tile-dispatch lambdas → RenderInner* take
  `const RenderContext&` and read FList/CAll/scene/target from it (closes the
  RenderInner remainder). Once renderFrame takes a `RenderContext` param, the
  offscreen callers stop swapping globals → Slice 6 parallelizes them.

  Pattern (proven): thread a param or extend the existing ctx; alias to
  locals so the body is untouched; pass the global at the call site (still
  canonical until renderFrame migrates); mirrortest byte gate
  (e440bfcdbe1aeb4e5f79bd1eba568459); commit per file.
```
