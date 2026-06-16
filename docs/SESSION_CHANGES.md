# Session changes — `feature/soa-vertex` (gbuffer worktree)

Handoff notes for a branch that wants to **merge or cherry-pick this work**. Covers the
last ~50 commits (`4a64c3c..af0b86d`). Grouped by theme, with the new public API surface,
new flags, validation status, and merge-order dependencies.

> The headline deliverables are **inter-render parallelism for mirror reflections**
> (per-worker offscreen renders fanned across the pool), an **optional deferred path**
> for those reflections (shadows + volumetric cones in mirrors), and a **parallelized
> static lightmap bake**. Everything is byte-gated and/or TSan-validated as noted.

---

## 1. Foundation — `RenderContext` / `DeferredOverride` de-globalization

The reflection-parallelism work needs the render pipeline to stop reading per-frame
**global** state (G-buffer pointer, view matrix, FOV, Z-scale, light lists) so that
several offscreen renders can run concurrently, each with its own target. These commits
thread that state through explicit parameters.

| commit | change |
|---|---|
| `821f93a` | Slice 3 — `DeferredLightLists` off globals |
| `d4e2e49` `e96b799` `2c82eb3` | deferred tile kernels read addressing from `ctx` (fov, zscale, fog tile) |
| `41fa4ab` | `RenderInner` mask gate reads `rt` (RenderTarget) |
| `39b6110` | volumetric cone + halo tiles take `ctx` |
| `5adda03` `0cf4caf` | forward `RenderInner` takes a per-pass `RenderContext` |
| `50ffe20` | `renderFrame` owns the deferred ctx; orchestrators take it by param |
| `3eb57cb` | `RenderInner` clips from `ctx.camera`, **not** the global scene |
| `0da7f61` | **`thread_local`** reflection-cull globals (`g_offAxisFrustumCull`, `g_reflVertCull`, `g_reflCone*`) |
| `2edfac6` | **`Render_DeferredLighting` takes an optional `const DeferredOverride*`** — the keystone |

### Public API surface a merging branch will collide with

```cpp
// FDS/RENDER/DeferredCommon.h
struct DeferredOverride {
    meka::GBuffer*            gb;            // target G-buffer (null → global main-frame)
    const fds::CameraContext* cam;
    ViewLightsSoA*            lights;
    TileLights*              tileLights;
    byte*                    vpage;
    word*                    zpage16;
    int                      xres, yres;
    meka::GBuffer*            gbXpar;
    word*                    xparZ, *xparZBack;
    bool                     inlineDispatch; // run tiles on the CALLING thread (no pool re-submit)
};

void Render_DeferredLighting(DeferredLightingCtx&, const DeferredOverride* ov = nullptr);
void Render_VolumetricCones (const DeferredLightingCtx&, bool inlineDispatch = false);
```

- When `ov == nullptr` the kernel reads the legacy globals exactly as before → **the main
  frame path is byte-identical** (this is what the gate proves).
- `inlineDispatch` is the mechanism that lets a pool worker run all its own tiles without
  re-entering the pool (no `renderns::tileDone` traffic, no deadlock from nested fan-out).
- `FrameState.{h,cpp}`: the reflection-cull globals are now `thread_local`. **If your branch
  added globals there, they must become `thread_local` too** or concurrent reflection
  renders will race on them.

---

## 2. `DeferredLighting.cpp` split (6 files)

Landed at the very start of the window (predates the parallelism work but is in range).
Pure structural move — `1dcfd4e..3cac793`, each step byte-gated:

`DeferredCommon.h` (structs/constants/inline helpers) · `DeferredLightLists.cpp` (per-tile
culling) · `DeferredShadowSampling.h` (per-pixel resolve) · `DeferredVolumetric.cpp` (fog,
cones, halos, skybox) · `DeferredFastFog.cpp` · `DeferredSurfaceKernel.cpp` (the remainder).

**Merge note:** if your branch edits the old monolithic `DeferredLighting.cpp`, expect a
hard conflict — re-target your hunks at the split files above.

---

## 3. Shadow flicker — torn read in the shadow-skip cache (`4fe3be2`)

Per-material shadow-skip was stored as **two relaxed atomics**; a pointer-slot collision
let one worker read a stale skip → intermittent tile flicker. Fixed by packing into a
single `atomic<uintptr_t>`. TSan was **blind** to it (it ignores atomic races); repro was
12 concurrent benches counting STEADY events. Self-contained one-area fix — low merge risk.

---

## 4. Mirror shatter + per-shard live reflections

| commit | change |
|---|---|
| `579ea3a` | mirror shatter with per-shard live reflections + RenderContext scaffolding |
| `6474f8f` | opt-in deferred shard bake + perf verdict (kept forward as default) |
| `a66668b` | fix R/B channel pairing in `TheOtherBarry` colorize (forward path) |
| `126c0b8` | greets bounce — window-portal test kills the through-wall surface leak |

`DEMO/MirrorShatter.{h,cpp}`: pimpl `ReflPool`/`ReflWorker` (each owns surf/cam/camCtx/
faces/scratch + gb/lights/tileLights).

---

## 5. Slice 6 — parallel forward reflection pass

| commit | change |
|---|---|
| `e5009b2` | parallel **per-worker forward** reflection pass |
| `2d9ae87` | **TSan fix:** skip the shared `BSphereScreenPos` write in offscreen passes |
| `fe54ece` | bench: greets 52ms → **6.3ms (8.3×, 12 workers)** |

The TSan fix (`2d9ae87`) is important: `Transform_Objects` wrote a **shared per-mesh**
field (`TriMesh::BSphereScreenPos`) from concurrent workers. Gated behind
`if (!_inShadowPass && !fds::g_offAxisFrustumCull)`. The `VertexScratch` clones were
already isolated; the hazard was this one shared-mesh write.

---

## 6. `--shard-deferred` — parallel **deferred** shard bake

| commit | change |
|---|---|
| `445493c` | parallel deferred shard bake (`FDS_SHARD_DEFERRED`) |
| `5bad86a` | promote to **`--shard-deferred`** FeatureFlag |
| `d8b4aae` `58ddc35` | disco/spot **volumetric cones** in deferred shard reflections (TSan-clean) |
| `1c2db61` | diag: `FDS_SHARD_ATLAS_DUMP` — de-tiled reflection atlas → PPM |

Shards bake through the deferred kernel (shadowed, lit exactly like the main view) instead
of the forward filler. ~20ms vs ~6ms forward on greets's 238 shards — **off by default**;
it buys shadowed quality for the cost. `FDS_SHARD_REFL_SERIAL` forces the legacy serial
path either way.

---

## 7. Deferred RTT recursive mirror (`8e0cb91`)

Second-order / recursive (RTT) mirrors get shadows + disco cones via the deferred path too
(static `s_rttGB`/`s_rttLights`/`s_rttTileLights`, gated on `shard_deferred()`).

> **OPEN / pending scoping (not yet committed):** deferred RTT should apply **only to the
> non-breakable screen mirrors**, not the geometry-based teleporter/portal mirror (where it
> only adds cost). A merging branch should be aware this gate is still being tightened.

---

## 8. Init-race fix (`0d51c6f`)

`LightmapBake_Static` (worker thread) raced `Scene_RebuildMatTable` rewrites. Fixed by
spawning the lightmap-bake thread **after** the `if (greets_mirror())` mirror/shatter block
in `Initialize_Greets`. TSan-confirmed. **Ordering-sensitive** — preserve the spawn site if
you touch `GREETS.CPP` init.

---

## 9. `--greets-mirror-rtt-min-area` (`92fae9a`)

`FDS_GREETS_MIRROR_RTT_MIN_AREA` (default **1.5**). Central column panels (~0.98–1.00 area)
and box edge strips (~0.30) are barely-visible half-silvered screens that each cost an RTT
slot + per-frame bake. Default drops them; only end-screen clones at ≥2.0 survive. Set
`0.2` to re-include.

---

## 10. Parallel static lightmap bake (`af0b86d`)

`FDS/RENDER/LightmapBake.cpp` — per-face loop wrapped in `bakeOneFace` lambda, fanned via
`atomic<uint32_t> cur` + `counting_semaphore` (threshold `nf >= 32 && P > 1`). Counters are
`atomic`; `SampleStaticCubeAtWorld` is read-only. **17.7s → 3.3s (5.3×).** TSan-clean.

---

## New flags introduced this session

| flag (CLI) | env | default | scope |
|---|---|---|---|
| `--shard-deferred` | `FDS_SHARD_DEFERRED` | `0` | greets — shards bake via deferred kernel |
| `--greets-mirror-rtt-min-area` | `FDS_GREETS_MIRROR_RTT_MIN_AREA` | `1.5` | greets — RTT-mirror area cutoff |
| (diag) | `FDS_SHARD_ATLAS_DUMP` | off | dump de-tiled reflection atlas to PPM |
| (existing knob) | `FDS_SHARD_REFL_SERIAL` | off | force serial shard path |

---

## Validation

- **Byte gate:** `tools/render_gate.sh` — `mirrortest` / `conetest` / `halotest` must match
  their golden md5s. Every de-globalization move-commit was gated **byte-identical** on the
  main-frame path. **Greets is NOT gate-covered** (mirrors/shards/cones live there) — it was
  validated by TSan + visual inspection.
- **TSan:** the forward parallel pass, the deferred shard pass, and the lightmap bake are
  all TSan-clean. Remember TSan is **blind to atomic torn-reads** (cost us the §3 flicker
  hunt) — for atomic-packed state, validate with concurrent-bench STEADY-event counting.
- **⚠ ThinLTO incremental-build nondeterminism:** editing a widely-included header
  (`DeferredCommon.h`) drifts cross-module inlining and produces **spurious** byte-gate
  failures. **Always clean-build before trusting the gate after a header edit.**

---

## Suggested merge order

The commits are layered; cherry-pick in dependency order:

1. **§1 RenderContext / `DeferredOverride`** — foundation; everything else assumes the
   `ov=nullptr` keystone and the `thread_local` cull globals.
2. **§2 Deferred split** — if you carry forward deferred-kernel edits, rebase them onto the
   split files first.
3. **§5/§6 shard parallelism** then **§7 deferred RTT** — depend on §1.
4. **§8 init-race fix**, **§9 RTT area cutoff**, **§10 lightmap bake** — largely independent;
   §8 is ordering-sensitive in `GREETS.CPP`.

---

## Known-open items (carry into the merge)

- Scope deferred RTT to the non-breakable screen mirrors only (§7).
- Tighten whole-mirror visibility culling further.
- Mirror **tint after break** (shards/screen lose the tint).
- **Side/back faces of the screen box surviving the shatter** (task #27).
- **Perf ground truth (instruction-level, greets t=700, full flags, 58.6ms RNDR / 93.6% of
  frame):** the **volumetric cone pass dominates** — `Render_VolumetricCones_Tile` ≈ 30.6k
  leaf samples, then per-pixel cube-shadow sampling (`CubeShadow_Sample`+`resolveCubeAtten`
  ≈ 24.5k), then the deferred surface kernel (≈ 24k). The shadow-map *bake* is modest
  (≈ 9.4k). Optimization should target the cone pass + per-pixel shadow taps, **not** the
  bake.
