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

**Slice 2 — per-context G-buffer (the high-value slice).**
Give `RenderContext` owned G-buffer allocation sized to its target. Add a
`Render(ctx, path)` overload that uses `ctx.target.gbuffer` instead of the
global. The shard bake creates a 64²-sized context and calls deferred →
shadowed, correct-magnitude reflections (closes the residual unshadowed gap
left after the R/B fix). **This is also where we finally measure the true
64² deferred per-render cost** (the open perf question — the full-frame 8×
ratio is per-pixel-bound and shouldn't extrapolate). If too slow at N=238,
the round-robin budget already built in `MirrorShatter::renderReflectionCameras`
absorbs it.

**Slice 3 — thread the context through the orchestration.**
`renderFrame`, `RenderInner*`, `Render_DeferredLighting` + volumetric/fog
passes, and `Transform_Objects`' projection reads take `const RenderContext&`
(or the sub-structs) instead of reading globals. One consumer at a time,
byte-compared. The hot inner loops (`Mekalele`, `TheOtherBarry`) already take
`rt`/`cam` — the work is the *passes that call them* and the per-pixel
deferred kernel's global reads (`FOVX`, `CntrEX`, `XRes`, the gbuffer).

**Slice 4 — thread `CurScene` → `ctx.scene`.** Mechanical; many call sites.

**Slice 5 — delete the globals.** Once nothing reads them, remove the
aliases. The `OffscreenViewScope` dance disappears (contexts don't swap
globals). CITY cube bake + mirror RTT adopt contexts.

**Slice 6 — parallelize the shard pass.** Fan the per-shard contexts across
the pool (atlas cells are disjoint → no write contention). Re-measure.

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

- Per-slice: 3-scene / 3-pose byte gate on the **primary** path (must stay
  identical for slices 1, 3, 4, 5).
- Offscreen quality: eyeball greets shatter + mirror RTT after slices 2, 6.
- Perf: `FDS_SHARD_REFL_PROF` (per-pass ms), `FDS_RNDR_BENCH` (forward vs
  deferred per-frame), `FDS_SHARD_REFL_LOOP` (extend runtime for sampling).
  Kept gated in the tree for this work.
```
