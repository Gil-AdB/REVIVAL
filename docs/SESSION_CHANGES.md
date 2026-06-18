# Session changes — `feature/soa-vertex` (gbuffer worktree)

Handoff notes for a branch that wants to **merge or cherry-pick this work**. Covers
`4a64c3c` onward (the de-globalization era through the greets-mirror tint + disco-shadow
polish in §11–§12). Grouped by theme, with the new public API surface, new flags,
validation status, and merge-order dependencies.

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

## 11. Greets shatter / mirror polish

| commit | change |
|---|---|
| `209c7bc` `3d5e930` | **black strip = box side/back faces wrongly registered as mirrors** — fixed at root: a glass cluster becomes a mirror only if it's a real display surface (reject `aspect>6` thin caps + `facesWall` backs); cluster by plane **and** spatial proximity (`kFixtureRadius=12`) so a real screen coplanar with a box cap isn't merged + dropped. Supersedes the abandoned EDGEBRICK repaint (`a5d709a`/`da588b0`). |
| `a9fcc95` | tunable shard **shape/size** (`greets_shard_randomness`), **fall speed** (`greets_shard_fall_speed`), **flat settle** on the surface beneath each shard (`greets_shard_lay_flat` + per-shard `castFloorAt` downward ray into static floor/stage tris). |
| (this batch) | **unified half-silvered tint** + **shard-atlas pre-bake** (below). |

### Unified mirror tint (`greets_mirror_tint`)

Every greets mirror now composites with the SAME formula — the half-silvered display
screens' (#2) `out = silver*tint + reflection/2`:
- **Display screens (#2):** native — deferred transparent kernel `litRGB + dst/2` (`XparBlendAlpha=0`).
- **Teleporter portal (#1):** same kernel path — `XparBlendAlpha=0` + `Lum=tint` (litRGB = silver*tint). Two real bugs fixed so any silver shows: the silver was a malformed **1×1 `Txtr_Tiled`** texture (→ 8×8) and the clone's inherited **`Lum=0`** lit it black.
- **Shatter shards (#3):** the same formula on the baked atlas pixels (`ApplyShardSilverGlaze`: `silver*tint + atlas/2`).

The half-silvered look is a **dimmed warm reflection, not an added silver colour** — so
`greets_mirror_tint` defaults to **0** (= `reflection/2`, matching #2 with no cast); raise
it for an optional cool-silver cast on #1 + #3. Replaces the old separate
`greets_shard_silver` / `greets_portal_tint`. `FDS_TINT_RED=1` swaps silver→red to verify.

> ⚠ The greets mirrors are **not faithfully renderable headless** (the teleporter
> reflection comes out black under `FDS_GREETS_CAM`, and the deferred shard bake is far
> darker than live) — tint changes **must** be verified in the live demo.

### Shard-atlas pre-bake

`MirrorShatter::enableReflectionCameras` split: the EXPENSIVE setup (offscreen surface +
atlas texture + per-shard material swap + `Scene_RebuildMatTable` + deferred-bake
G-buffers + worker pool) moves to `prepareReflectionAtlas()` at **init**; `setShardText()`
+ `armReflectionCameras()` (cheap) run on the break. Shrinks the ~40ms one-frame hitch at
shatter (cold-bake residual remains). **Ordering:** `prepareReflectionAtlas` runs inside
the `Initialize_Greets` mirror block — preserve it relative to the §8 lightmap-thread spawn.

---

## 12. Disco ball non-shadow-casting (`Tri_NoShadowCast`)

New general `TrimeshFlags` bit `Tri_NoShadowCast` (`1<<15`, `FDS/Base/FDS_DEFS.H`): a mesh
flagged with it lights normally but is **skipped in every shadow occluder pass** — gated in
`Transform_Objects` on `g_inShadowPass` (covers the static cube bake, the dynamic per-frame
bake, AND the moving-omni cube re-bake; the static lightmap samples those same maps, so it's
covered transitively). `BuildDiscoBall` sets it on the ball (`DEMO/GreetsDisco.cpp`) so its
faceted sphere stops throwing a hard blob shadow that read as a dark hole. Reusable by any
mesh that should cast no shadow.

---

## 13. Greets perf + bounce shadow + multi-atlas — commits `0ce5373..f229044`

Five commits layered **on top of `bf0b6b5`** (the merge-base with `feature/fog`). These are
the ones a fog merge has NOT yet seen.

- **`0ce5373` SoA dump trim** — `VertexFrame_DumpFromAoS` writes only the consumed field
  (`TPos_z`), not all 18. `FDS/Base/VertexFrame.cpp` + `Transform.cpp`. Low conflict risk.
- **`2c9843a` mirror-bounce through-wall shadow fix** — bounce spots (`Omni_BounceCone`,
  `castsShadow=false`) borrow their source disco spot's reflected 2-D map. Extends the
  `srcShadowMapIdx` gate to `Omni_BounceCone`, adds `bounceClamp` (default-dark for bounce,
  default-lit for clones) in `DeferredSurfaceKernel.cpp`, plumbs `bounceClamp` through
  `TileLights` (`DeferredCommon.h` + `DeferredLightLists.cpp`), and relaxes the volumetric
  source-tap gate in `DeferredVolumetric.cpp`.
- **`fdd1653` mirrortest golden refresh** — `tools/render_gate.sh` `BASE_MIRROR` →
  `bd664b1067b6610b486d1c81b305ebf6` (was stale pre-existing drift, unrelated to the fix).
- **`06c225d` view-dependent mirror RTT res** — `RenderSecondOrderMirrors` sizes each bake
  to the panel's on-screen footprint (clamped to a build-time max `texWMax/texHMax`),
  re-pointing the texture `SizeX/LSizeX` per job. ANIM 45.6→2.7ms at the greets central room.
  Flags `--mirror-rtt-adaptive` (default on) + `--mirror-rtt-adaptive-scale`. `GreetsMirror.cpp/.h`.
- **`f229044` multi-atlas shard res** — shards split across multiple ≤1024² reflection
  atlases (the block-tiled sampler `tile_u/tile_v` overflows past 1024/axis → black above a
  single-atlas's 64). New `--greets-shard-res` (pow2, default 64); atlas count capped at 8.
  `MirrorShatter.cpp/.h` (`atlasTex_`/`atlasMat_` → vectors + `shardCell()`), `GREETS.CPP`.

### ⚠ Conflict surface vs `feature/fog` (7 files; 3 hard)

The HDR work and this work touch overlapping code. Resolutions (the changes are **orthogonal
in intent** — res/shadowing vs colour-space — so the merge is usually "keep both"):

- **`GreetsMirror.cpp` (`RenderSecondOrderMirrors`)** — adaptive res sizing (this) vs
  HDR-correct RTT tonemap routing (fog). Keep both: size the bake adaptively **and** run it
  through the HDR path. The per-job `s.texW/texH = pow2clamp(footprint)` block is independent
  of the colour-space routing.
- **`DeferredSurfaceKernel.cpp`** — the `srcShadowMapIdx`/`bounceClamp` shadow branch (this)
  is independent of linear-space lighting math (fog); both apply.
- **`DeferredVolumetric.cpp`** — bounce source-tap gate relax (this) vs HDR cone compositing
  (fog); orthogonal.
- **`DeferredCommon.h`** — `bounceClamp` field added to `TileLights`/`ViewLightsSoA` (this)
  vs HDR ctx fields (fog). ⚠ widely-included header → **clean-build before trusting the gate.**
- **`FeatureFlags.def`** — adjacent flag additions; auto-merges, just check for no dup.
- **`GREETS.CPP`** — `--greets-shard-res` wiring (this) vs disco HDR-emissive + tonemap hooks
  (fog). Note fog **retired `DiscoBloomPost` (`12e8c8b`)** — drop any reference to it.
- **`tools/render_gate.sh`** — BOTH branches changed `BASE_MIRROR` (fog `8c84efc`, this
  `fdd1653`). After the merge the mirrortest output changes again (HDR tonemap × adaptive
  res), so **re-baseline ONCE post-merge and agree on the single golden hash.**

### Threading / DAG work is BLOCKED on this consolidation

The next campaign (fuse lighting+cones, overlap shadow-bake ∥ gbuffer, eventual task-DAG)
restructures the **same `RENDER.CPP` orchestration** fog just threaded in `317a4d6` (tonemap
6×4 tile-job dispatch). Do NOT start it until the consolidated base (with `317a4d6`) lands on
`soa-vertex`, so the fusion is built **on top of** the threaded tonemap (folding the tonemap
into the fused per-tile pass) rather than colliding with it.

## New flags introduced this session

| flag (CLI) | env | default | scope |
|---|---|---|---|
| `--mirror-rtt-adaptive` | `FDS_MIRROR_RTT_ADAPTIVE` | `1` | greets — RTT bake sized to on-screen footprint |
| `--mirror-rtt-adaptive-scale` | `FDS_MIRROR_RTT_ADAPTIVE_SCALE` | `1.0` | greets — RTT footprint sharpness multiplier |
| `--greets-shard-res` | `FDS_GREETS_SHARD_RES` | `64` | greets — per-shard reflection res (pow2; multi-atlas) |
| `--shard-deferred` | `FDS_SHARD_DEFERRED` | `0` | greets — shards bake via deferred kernel |
| `--greets-mirror-rtt-min-area` | `FDS_GREETS_MIRROR_RTT_MIN_AREA` | `1.5` | greets — RTT-mirror area cutoff |
| `--greets-mirror-tint` | `FDS_GREETS_MIRROR_TINT` | `0.0` | greets — optional cool-silver cast on all mirrors (`silver*tint + reflection/2`) |
| `--greets-shard-randomness` | `FDS_GREETS_SHARD_RANDOMNESS` | `1.0` | greets — shard shape/size irregularity |
| `--greets-shard-fall-speed` | `FDS_GREETS_SHARD_FALL_SPEED` | `1.5` | greets — shard gravity multiplier |
| `--greets-shard-lay-flat` | `FDS_GREETS_SHARD_LAY_FLAT` | `1` | greets — settle shards flat on the surface beneath |
| (diag) | `FDS_SHARD_ATLAS_DUMP` | off | dump de-tiled reflection atlas to PPM |
| (diag) | `FDS_TINT_RED` | off | swap mirror silver → red to verify the tint lands |
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
- ~~Mirror tint after break~~ — **resolved** (§11 unified tint).
- ~~Black strip from box side/back faces~~ — **resolved** at root (§11, `209c7bc`/`3d5e930`).
  The faces still exist as plain glass geometry after the break (no longer mirrors, no longer
  carving the wall); dropping the screen-box fixture mesh on shatter is a possible follow-up.
- **Perf ground truth (instruction-level, greets t=700, full flags, 58.6ms RNDR / 93.6% of
  frame):** the **volumetric cone pass dominates** — `Render_VolumetricCones_Tile` ≈ 30.6k
  leaf samples, then per-pixel cube-shadow sampling (`CubeShadow_Sample`+`resolveCubeAtten`
  ≈ 24.5k), then the deferred surface kernel (≈ 24k). The shadow-map *bake* is modest
  (≈ 9.4k). Optimization should target the cone pass + per-pixel shadow taps, **not** the
  bake.
