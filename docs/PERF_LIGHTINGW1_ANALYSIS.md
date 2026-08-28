# `lighting-w1` in CITY — `Render_DeferredLighting_Tile_OuterVec`

**Round:** read-only analysis, branch `rev-w1analysis`, off `fog-wt` @ `e017d611`.
**Target:** `Render_DeferredLighting_Tile_OuterVec`, `FDS/RENDER/DeferredSurfaceKernel.cpp:5766-6690`.
**Arm:** `./DEMO --snapshot=city@t=1961 --env_live_water --deferred --city_env_pixel`, 1512x848.
**Row:** `lighting-w1` **0.957 Gi/f = 22.9 % of `renderFrame`** (4.174 Gi/f), 15.3 % symbol
self-time, row 2 of `docs/PERF_STATE.md:1702`. Named as untouched territory by
`docs/OPTIMIZATION_BACKLOG.md:638`.

**NOTHING IN THIS DOCUMENT WAS MEASURED BY THIS ROUND.** No build, no run, no timing —
another agent owns the machine. Every number below is either (a) quoted from an existing
doc, with the line number, or (b) an *instruction count read off the source*, labelled
"static estimate". Static estimates are hypotheses. The instrument that would falsify each
one is named with the candidate.

---

## 0. The three facts that reframe the row

Before the walkthrough, three structural facts that most of the campaign's intuition about
`lighting-w1` does **not** survive:

1. **This kernel takes no shadow tap of any kind.** Not a cube tap, not a 2-D spot tap,
   not in the vec body and not in the scalar fallback. `grep -n 'hadow'` over 5766-6690
   returns zero hits outside comments. `docs/PERF_STATE.md:2851-2858` states it
   independently. Greets' `lighting-w1` is 36.6 % cube tap
   (`docs/OPTIMIZATION_BACKLOG.md:4007-4035`); city's is **0 %**.
2. **`--pbr` is structurally inert here.** `fds::FeatureFlags::pbr()` is read at exactly
   one place in the file — `DeferredSurfaceKernel.cpp:2285`, inside
   `Render_DeferredLighting_TileT`, the *scalar* kernel. The OuterVec kernel has no GGX
   lobe, no Smith-Schlick geometry, no `run_vec_ggx_loop`. Its only analytic specular is
   `std::pow(NdotH, gloss)` at `:6497`, in the per-lane scalar redo. `--pbr` on vs off
   changes nothing in this row. (`--pbr` *is* in `PRESETS/city-noir.flags`; that preset
   does not pass `--deferred`, so it renders forward and never reaches this kernel.)
3. **The row's mirror machinery is provably dead in city.** `gb.mirrorId` is allocated in
   exactly one place in the tree — `FDS/RENDER/GreetsMirror.cpp:1171` — so in city
   `gb.mirrorId.empty()` is true; and `FDS/RENDER/ReflMirror.cpp:140-143` says of
   `--refl_correct`'s mirrored light state, verbatim: *"mirrorId — deliberately left at 0.
   These are not clone lights: ... city does not allocate `gb.mirrorId`, so the per-pixel
   `pmid` is hard 0"*. Both sides of the per-light mirror compare are therefore constant
   zero at every pixel of every city frame.

---

## 1. What the outer-vec kernel does

### 1.1 Call path

`renderFrame` -> `Render_DeferredLighting` (`:7588`) -> per-frame setup (view-space light
SoA build `:7695-7760`, `buildTileLightLists` `:7936`) -> `TailProf::Stamp _w1q("lighting-w1")`
`:8158` -> `dispatchIndexed` over the 12x8 = 96 lighting tiles `:8181` ->
**`Render_DeferredLighting_Tile_OuterVec(ctx, t, x1,y1,x2,y2)`** `:8186`.

The selector is `deferredLightingOuterVecEnabled()` (`:5539-5546`): explicit
`--deferred_outer_vec` wins, else `CurScene->PreferOuterVec != 0`. City sets it at
`DEMO/CITY.CPP:2586` (*"Matte-dominated scene — OuterVec's outer-SIMD wins by ~3 ms"*),
crash at `CRASH.CPP:25`, fountain at `FOUNTAIN.CPP:1029`. Greets and chase leave it 0 and
run `Render_DeferredLighting_Tile` -> `Render_DeferredLighting_TileT<bool>` (`:2148`).

Tile geometry: `DEFERRED_NUM_TILES_X/Y = 12/8` (`DeferredCommon.h:76-78`),
`DEFERRED_MAX_LIGHTS = 128` (`:67`), and `tileSizeX` is rounded up to a multiple of 8
*specifically for this kernel* (`:7890-7894`). At 1512x848 a tile is 128x106 px = 16
groups x 106 rows = **1 696 eight-pixel groups per tile**.

### 1.2 What makes it "outer vec"

**It is vectorised over PIXELS, not over lights.** One `__m256` holds 8 horizontally
adjacent pixels of one scanline; the light loop at `:6222` is a *scalar* loop over
`tl.count` that broadcasts one light and FMAs it into all 8 pixels:

```
for (int n = 0; n < omniLoopN; ++n) {
    __m256 wx = _mm256_sub_ps(_mm256_set1_ps(tl.posX[n]), xv);   // 1 light, 8 pixels
    ...
    lB = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colB[n]), lB);
}
```

The *other* SIMD kernel in this file — `--deferred_vec`, inside `TileT` at `:3135-3320` —
is the transpose: **8 lights, 1 pixel** (`for (int slot = 0; slot < tl.paddedCount; slot += 8)`,
`:2148+988`). It is `FDS_DEFERRED_VEC_DEFAULT = 0` on arm64 and is not what greets runs.

So there are three kernels, not two:

| | axis | who runs it | shadows | PBR/GGX |
|---|---|---|---|---|
| `TileT` scalar body | none (1 px, 1 light) | **greets, chase** | full (cube + 2-D + lightmap) | yes |
| `TileT` inner-vec body | **8 lights** x 1 px | nobody at arm64 defaults | yes (lane-scalarised) | yes |
| `_Tile_OuterVec` | **8 px** x 1 light | **city, fountain, crash** | **none** | **none** |

### 1.3 Per-group loop structure

The kernel is `for (py) { for (px += 8) { ... } }`. Everything below is *per 8-pixel group*
unless marked. Line numbers are the section heads.

| # | stage | lines | shape |
|---|---|---|---|
| a | per-lane `mirrorId` widen (8 scalar stores) | 5892-5897 | scalar x8 |
| b | Z load, alive mask, lane-in-range mask, checker/quarter masks | 5899-5948 | vec |
| c | `mat32` load, matID extract, bounds mask, 2 scratch stores | 5950-5962 | vec |
| d | **per-lane material gather** | 5964-6039 | **scalar x8** |
| e | octahedral normal decode + rsqrt + 1 Newton | 6041-6084 | vec |
| f | **per-lane normal-map / TBN** | 6086-6148 | **scalar x8** |
| g | `--sh_ambient` irradiance (off by default) | 6156-6173 | **scalar x8** |
| h | view-space position reconstruct | 6174-6194 | vec |
| i | texel/ambient vec loads, water + `needScalar` masks, `anyVecLane` | 6196-6217 | vec |
| j | **the omni loop** | 6219-6287 | vec, `tl.count` iterations |
| k | saturate to 250, `tex*lit/256` | 6288-6308 | vec |
| l | `needsScalar` mask, 11 scratch store-outs | 6309-6335 | vec |
| m | 8-wide env front-end (`EnvComposeCityVec8`), uniformity scan | 6336-6395 | scalar scan + vec |
| n | **pack loop** — per lane: scalar redo, or env-only add, or plain pack | 6397-6666 | **scalar x8** |

Stage (d) resolves, per lane: `matTable.data[matID]` -> `Mat->Txtr` -> `Txtr->Mipmap[mip]`
-> `texData[uv]` (three dependent loads before the texel gather), then reads `TintB/G/R`
(`Material.h:250-252`), `Luminosity`/`Diffuse`/`Specular`/`Glossiness` (`:92-95`),
`SpecMul` (`:266`), `Reflection` (`:282`), `MetallicMap` (`:277`), and writes 13 scratch
arrays. `docs/OPTIMIZATION_BACKLOG.md:5693` measured `sizeof(Material) = 467 B` with the
per-pixel fields spread over **lines 0/1/3/4/6** and flagged the "hot fields on line 0"
comment as stale. This kernel pays that walk up to **three times per lane** (stage d,
stage f's `MatN`, stage n's `Mat_`).

Stage (n) is a three-way branch per lane:
* `lane_needs_scalar[k]` (= `wantSpec | isWater`) -> **full scalar re-shade** `:6401-6553`:
  a second complete `for n < tl.count` loop with N.L, range, spot cone, diffuse *and*
  `std::pow(NdotH, gloss)` specular — **even though the vec loop at (j) already accumulated
  this lane's diffuse** (`omniMask` at `:6202` masks off water but *not* `wantSpec`).
  `vfB/vfG/vfR[k]` is computed and discarded for every such lane.
* env-only lane with `envVecReady` -> `:6561-6614`: face pick, live-water tilt,
  `EnvCubeFetchBil` x1-2.
* env-only lane without -> `EnvSpecComposeScalar` `:6615-6645`.
* otherwise -> `int()` truncate, clamp, one `out[i+k]` dword store.

Note the pack writes **eight separate 32-bit stores**, never one 256-bit store, even when
all eight lanes take the plain path.

### 1.4 Transport semantics — why this kernel is not the scalar one

`docs/OPTIMIZATION_BACKLOG.md:15-24` (round 2026-08-26), verbatim:

> `Render_DeferredLighting_Tile_OuterVec` ... **stores 8-bit VPage only and deliberately
> leaves the HDR coverage lane `h[3]` at 0**: its pack *is* the HDR transport, lifted
> afterwards by the froxel composite (`h[3] > 0 ? h : VPage`) or by `Hdr_ActivateNoFog`.
> The SCALAR wave-1 kernel that greets and chase run (PreferOuterVec 0) does write
> `g_hdrBuf` and stamp coverage.

The predicate is `Deferred_KernelWritesHdrRadiance()` = `!deferredLightingOuterVecEnabled()`
(`:5558`, declared `DeferredCommon.h:405`). The source carries the consequence as a warning
at the kernel head, `:5814-5821` — **"DO NOT PORT `--deferred_shade_ldr_skip` HERE"** —
and the same fact bit the checkerboard fill (`:6736-6754`, `&& !outerVecG`) and the SSAO
apply (`--ssao_hdr_transport`, `FeatureFlags.def:549`).

**This is the single biggest constraint on transfer**: any lever whose safety argument is
*"the 8-bit compose is dead because the tonemap overwrites VPage"* is false here. In this
kernel the 8-bit compose is the only thing that exists.

---

## 2. What the greets rounds won, and whether it transfers

Eleven landed rounds touched greets' `lighting-w1` / omni loop. Mapping each to OuterVec:

| round | lever (one line) | structure in OuterVec | transfers? |
|---|---|---|---|
| **2026-08-15** (`:4652-4834`) | hoist `computeMapShadowAtten`'s own three `if`s to the call site — 99.50 % of 4.749 M calls carry no shadow index. greets `lighting-w1` **-9.0 %**, chase **-22.5 %** | function is **never called**; no `shadowMapIdx`/`srcShadowMapIdx`/`srcCubeShadowIdx` read exists in 5766-6690 | **NO — structurally absent.** The round measured city itself: `lighting-w1` **1.146 -> 1.145**, i.e. zero (`:4754`) |
| **2026-08-15b** (`SESSION_STATE.md:2770-2966`) | 8x8 PolyId uniformity pyramid; 80.3 % of taps skip; greets **-0.192 Gi/f (-6.5 %)** | no taps | **NO.** *"chase / city / fountain build no shadow maps at all and are structurally inert"* |
| **2026-08-15c** (`:4813-4824`) | pyramid reader in the 2-D spot tap | no spot tap | **NO** |
| **2026-08-14** (`:4988-5169`) | `--deferred_tile_sphere_cull` — 3-D sphere-vs-tile-chunk cull replacing rect AND z-extent. greets `lighting-w1` **-3.3 %** | in `buildTileLightLists` (`DeferredLightLists.cpp:246`), **before** either kernel | **ALREADY HAS IT.** Default ON (`FeatureFlags.def:31`); city has been getting it since it landed. The only shared, scene-agnostic landing in the set |
| **16g** (`:3987-4190`) `--deferred_cube_direct` | call `CubeShadow_Sample` with 8 args instead of `resolveCubeAtten`'s 20; the whole w1 win of that round | `resolveCubeAtten` not called | **NO** |
| **16g** `--deferred_lm_addr_skip` | skip the static-lightmap address resolve | no lightmap resolve in this kernel | **NO** |
| **16g** `--deferred_fill_hdr_skip` | wave-2 fill: 3 dead divisions per neighbour under `--hdr` | wave 2 only; and under OuterVec the fill's `hdrWrite` is forced false at `:6753` | **NO** |
| **16h** (`:3826-3986`) material hoist in `fetchTexelNb` | reuse the centre's `Texture*` when `neighborCompatible` already proved matID equality; **-1.0 % of w2**, flagless | wave 2 only — **but the IDEA transfers, transposed**: see candidate C3. OuterVec re-resolves the *same* `Material*` up to 3x per lane and the *same* matID across up to 8 lanes | **RE-DERIVE** |
| **16h** `--deferred_fill_ldr_skip` | skip the fill's 8-bit average under `ctx.ldrDiscarded`; **-6.2 %** | wave-2; and inert on any PreferOuterVec scene by construction (`:6753` clears `hdrWrite`, which clears `fillLdrSkip`) | **NO** |
| **16i** (`:3629-3825`) `--deferred_shade_ldr_skip` | wave-1's 8-bit compose chain is dead under `--hdr --hdr_linear`; **-1.78 %** | **EXPLICITLY FORBIDDEN.** `:3764-3770` and the source comment at `:5814-5821`. The 8-bit pack IS the HDR transport here | **NO — and it is a bug if anyone ports it** |
| **16l** (`:2991-3272`) `--deferred_cube_prepass` | the cube tap's 61 %-of-204-instruction projection prologue moved to a per-tile-row 8-wide sweep, *because fixed-light-x-8-pixels turns a gather into a broadcast*. greets **-4.1 % Gi, -13.8 % Gcyc** | no tap to prologue. **BUT the round's own insight — "8 px per light makes light state a broadcast" — is what OuterVec already IS.** OuterVec is the shape 16l retrofitted onto the scalar kernel | **ALREADY HAS THE SHAPE, has nothing to apply it to** |
| **16m** (`:2794-2990`) publish `ShadowSwzGetShape()`'s lazy static to a plain global | one never-taken `bl __cxa_guard_acquire` behind a function-local `static` pinned 8 callee-save pairs + a 144 B frame across five early rejects; removing it made the tap a leaf. **greets `lighting-w1` Gcyc/f -1.17 to -2.85 % at 5/5 poses**, ~30 lines, bit-exact, flagless | **THE IDENTICAL DEFECT EXISTS HERE**, inside the per-group loop: `DeferredSurfaceKernel.cpp:6342`, `static const bool sEnvVecDiagOff = !std::getenv(...) && ... && !EnvTraceGet();` | **TRANSFERS DIRECTLY — this is candidate C1** |
| **16n** (`:2544-2793`) `always_inline` `computeMapShadowAtten` | delete ~46 instructions of 17-argument ABI per call; greets **-2.00 to -4.27 %** | function never called. The round measured city under his arm, 11 rounds: `lighting-w1` **+0.00 %**, `renderFrame` **+0.02 %** (`:2709-2712`) | **NO** |
| **16o** (`:2302-2543`) `oct_decode_u32_x4` | three scalar oct decodes -> one 4-wide, centre on lane 2 so its scalars die immediately. **w2 -11.9 to -13.4 %**; *"the answer is a live range, not a lane count"* | **ALREADY HAS IT, better**: OuterVec decodes 8 normals fully 8-wide inline at `:6041-6084`, with the `rsqrt` + 1 Newton refinement 16o's scalar path lacked | **ALREADY HAS IT** |
| **16z** (`:520-566`) live-water `slopeFn` | devirtualise the indirect call in *this kernel's* per-lane loop | present, and the only thing anyone has priced inside OuterVec | **REFUTED — see §4** |

### The verdict on transfer

**Six of eleven landings are shadow-tap work and city takes no shadow taps. Three are
wave-2 fill and city runs no checkerboard. One is explicitly forbidden. One (sphere cull)
city already has. One (oct x4) OuterVec already exceeds.**

What is left is exactly what `OPTIMIZATION_BACKLOG.md:638` meant and what
`docs/OPTIMIZATION_BACKLOG.md:4022-4033` itemised for greets — **the per-pixel FLOOR**
(0.473 Gi/f there: view pos + SH 0.104, normal decode + TBN 0.100, compose 0.060, view dir
0.038, AO 0.036, POM horizon 0.034, mat32 decode 0.029, loop floor 0.027, albedo 0.020,
world pos 0.017, lightmap address 0.008) — **plus two things greets does not have at all**:

* a **scalar per-lane gather** run 8x per group (greets' scalar kernel resolves the
  material once per pixel; OuterVec resolves it up to 3x per lane), and
* a **duplicated light loop**: every `wantSpec` lane pays the vec accumulate *and* a full
  scalar re-accumulate of the same diffuse.

Those two sites are the transferable territory, and neither has a round.

---

## 3. The per-pixel cost shape in CITY

### 3.1 The arm's flag state (checked against `FeatureFlags.def` defaults)

`--env_live_water --deferred --city_env_pixel` and nothing else, so:

| flag | value | consequence in this kernel |
|---|---|---|
| `hdr` | **0** | `hdrWrite` false -> the 250-saturate at `:6289-6294` **is** applied |
| `texture_filter` | **0** (`:153`) | `texFilterOn` **false** -> the texel is a real dependent gather `texData[uv]`, not `gb.albedo[i+k]` |
| `pbr` | 0 (`:48`) | inert anyway (§0.2) |
| `env_refl` | **forced to 1** by `DEMO/REV.CPP:1584-1587` because `city_env_pixel` is on | `envTabG != nullptr`; the env compose is live |
| `deferred_checkerboard` / `_quarter` | 0 | `checker`/`quarter` false; `checkerEnvFullOV` false; **wave 2 never runs** |
| `roughness_map` / `metal_map` | 1 / 1 (`:191`, `:165`) | globals on, but city's materials carry neither map, so the per-material tests all fall through |
| `sh_ambient` | 0 (`:56`) | the per-lane scalar SH loop at `:6156-6173` is skipped |
| `shadows` | `FDS_SHADOWS_DEFAULT_ON` | irrelevant — no tap in this kernel |
| `deferred_tile_sphere_cull`, `spot_cone_cull`, `deferred_zcull` | 1,1,1 | live, in list-building |

### 3.2 Lights per tile

`docs/OPTIMIZATION_BACKLOG.md:6227`: greets **6.9 lights/tile -> 20.9 ms**, city **26.1
lights/tile -> 3.4 ms**. `docs/PERF_STATE.md:44` refers to *"one pass over ~40 lights"* for
the scene-level list. So the tile list is ~26 entries and the light loop at `:6222` runs
~26 iterations per 8-pixel group. That 6x cost gap at 3.8x the lights is the whole reason
this row reads differently from greets': **city's per-light work is cheap and its per-light
COUNT is high**, which inverts which levers pay.

### 3.3 Per-light instruction budget (static estimate, AArch64)

simde lowers each `__m256` op to two 128-bit NEON instructions. Counting the non-spot body
at `:6223-6287`:

| term | `__m256` ops | NEON instr |
|---|--:|--:|
| 3 broadcasts posX/Y/Z + 3 subs | 6 | 12 |
| `dot` (2 fmadd + 1 mul) | 3 | 6 |
| `len2` (2 fmadd + 1 mul) | 3 | 6 |
| broadcasts range2, rRange, mirrorId | 3 | 6 |
| 3 float cmps + 1 int cmpeq | 4 | 8 |
| 4 ands | 4 | 8 |
| `safe_len2` blend, `rsqrt`, `dist` | 3 | 6 |
| falloff (mul + sub), `k` (2 mul) | 4 | 8 |
| `intensity` (mul + blend) | 2 | 4 |
| 3 colour broadcasts + 3 fmadd | 6 | 12 |
| **total** | **38** | **~76** |

= **~9.5 instructions per (pixel x light)**. At ~26 lights that is **~250 instructions per
shaded pixel in the omni loop alone**.

Cross-check against the measured row: 1512x848 = 1.282 Mpx; if ~70 % are alive and the row
is stamped twice per frame (the `x2` annotation on `PERF_STATE.md:1702` — **I have not
verified what `xN` means in that table and a measurement should settle it**), that is
~1.8 M shaded px for 0.957 Gi = **~530 instructions per shaded pixel**. The omni loop's
~250 is then ~47 % of the row, leaving ~280 for the per-pixel floor + the pack. That is
consistent, not proven. **Every prediction below is expressed as a % of the row, which is
invariant to the `xN` question; `% of renderFrame = % of row x 0.229`.**

### 3.4 Invariant per tile vs recomputed per pixel

**Recomputed per 8-pixel GROUP that is invariant per tile or per light:**

* `_mm256_div_ps(1.0f, cosInner[n] - cosOuter[n])` at `:6273-6274` — a **256-bit divide**
  (2x `fdivq`, non-pipelined, ~10-14 cycle latency) computed inside the innermost loop for
  a quantity that is constant for the light. 1 696 groups/tile x (spot lights in the tile)
  x 96 tiles x passes.
* `fds::FeatureFlags::prof_no_lights()` at `:6220` — a global-array read per group, in a
  kernel whose own head hoists 15 other flags and whose sibling `TileT` carries the comment
  *"Hoist mode/global queries once per tile ... 2M function calls/frame adds up"* (`:2175-2178`).
* `envPosFakesOff` at `:6355-6358` — **three** `FeatureFlags` reads per group.
* `sEnvVecDiagOff` at `:6342` — a function-local `static` with a dynamic initializer, i.e.
  a guard-variable acquire-load per group and a `bl __cxa_guard_acquire` in the function.
* the env-uniformity scan at `:6363-6369` — an 8-iteration `for k` loop that runs on
  **every** group, including the majority with no env lane at all, even though
  `lane_hasEnv[]` was already built at `:6034`.
* `lane_mirrorId[k] = 0u` x8 at `:5892-5893` — eight stores of a constant, then a vec
  reload at `:6222`, then a broadcast + cmpeq + and per light, for a mask that is
  **provably all-ones in city** (§0.3).

**Genuinely per pixel:** the Z decode, the mat32/uv decode, the texel gather, the normal
decode, the position reconstruct, the light accumulate, the pack.

**Genuinely per (pixel x light):** everything in the table of §3.3.

### 3.5 Shadow / cube-tap cost inside this row

**Zero.** See §0.1. City builds no shadow maps (`SESSION_STATE.md:2781`), makes zero
`computeMapShadowAtten` calls (`OPTIMIZATION_BACKLOG.md:2580-2584`), and takes zero cube
taps through the prepass (`:3221-3230`). `docs/PERF_STATE.md:2851-2858` adds that even the
OuterVec *scalar fallback* omits both attenuation calls.

Consequence: greets' `lighting-w1` is 36.6 % cube tap and 8.6 % GGX lobe; **city's is
~47 % Lambertian omni accumulate, ~35 % per-pixel floor + per-lane gather, ~13 % env
compose, ~5 % pack** (static estimate; the 13 % is anchored on the measured
`--city_env_pixel` +0.129 and env-compose +0.089 of `PERF_STATE.md:1702` and `:1457-1460`,
which give 0.715 plain `--deferred` -> 0.844 with glass in the G-buffer -> 0.933 with the
env compose -> 0.974 with `--env_live_water`).

### 3.6 Transcendentals and divisions

* `std::pow(NdotH, gloss)` — `:6497`, **scalar redo lanes only**, once per (spec-pixel x
  accumulating light). The only libm call in the kernel.
* `_mm256_div_ps` — `:6273`, per (spot light x group), invariant (§3.4).
* one scalar `/` — `:6444`, `(cosTheta - cosOuter)/(cosInner - cosOuter)`, but guarded by
  `if (cosTheta < cosInner)` so it only fires in the penumbra.
* `_mm256_rsqrt_ps` — `:6248`, the raw estimate, **no Newton step**.
* `fast_rsqrt` (`FILLERS/SimdHelpers.h:16`) — `vrsqrte` + **one** Newton step, in the
  scalar redo at `:6414`, `:6444`, `:6467`.

**Note the asymmetry**, which matters for candidate C6: the vec light loop's `lenInv` is a
bare `vrsqrteq` (the kernel's own comment at `:6043-6053` puts it at +-0.3 % and
*piecewise-constant*), while the scalar redo's is refined to +-6e-5. A vec lane and a
scalar lane on the same surface under the same light therefore compute measurably different
diffuse falloff. This is the same class of defect as `--vec_ggx_refine`
(`FeatureFlags.def:663`), one loop over.

### 3.7 The scalar-diversion branch

`--deferred_vec_force`'s description (`FeatureFlags.def:34`) — *"force the vec path even
for normal-mapped / AO-mapped pixels that normally divert to scalar"* — describes the
**scalar** kernel's `useVecHere` predicate at `:2148+791`:
`useVec && (sVecForce || (!hasNormalMap && !(hasAoMap && !aoInAlpha)))`. That flag has **no
effect on OuterVec**, whose diversion predicate is entirely different and is at `:6313-6317`:

```
needsScalar = lane_wantSpec | lane_isWater
lane_wantSpec[k] = (Mat->Specular > 0.0f && Specular_Factor > 0.0f)
```

`Specular_Factor = 1.0` is set unconditionally for city at `DEMO/CITY.CPP:3655` with the
comment *"many materials with authored `Mat->Specular`"*. So **a city pixel diverts to the
full scalar re-shade iff its material has `Specular > 0`.**

What I can say by static reasoning:

* The `--city_env_pixel` glass clones are **not** diverted: `CITY.CPP:3204` sets
  `clone->Specular = 0.0f` (and `Diffuse = 0.0f`, `Luminosity = city_env_lum`). Glass takes
  the `else` branch at `:6555` and pays only the env compose. This is deliberate and is why
  `EnvComposeCityVec8` exists.
* Normal maps do **not** divert here (they are handled in-place at `:6086-6147`).
* Water (`matID == ctx.waterMatID`) diverts, but city's water surface is drawn by the
  forward/reflection path, so its screen share in the deferred pass is small.
* Therefore the diversion rate **is exactly the screen coverage of city materials with
  authored `Specular > 0`** — road surfaces, vehicle bodies, metal trim. I could not bound
  it statically: the values live in the binary `Runtime/SCENES/CITY.FLD`.

**The measurement that answers it already exists and is committed:**
`--deferred_gloss_stats` (`FeatureFlags.def`, read at `DeferredSurfaceKernel.cpp:7656-7676`)
prints `specMats=<n> distinct={gloss:count,...}` once per scene. That gives the material
count. For the **pixel** count the cheapest instrument is a `noinline` reporter counting
`lane_needs_scalar[k]` at `:6399` — do not commit it (see §5, "instrument hygiene").

This number gates candidate C6, which is the largest on the list and the only one whose
size I cannot bound without it.

---

## 4. Prior refutations — do not re-propose

| idea | where | why it died |
|---|---|---|
| **finer light grid** | `BACKLOG:4137-4144` | Analytic, no build. A light a finer tile would remove is one the per-pixel `len2 > r2` already rejects — census puts that at **8.36 % of pairs**; stages 2-4 cost 25.7 instr/pair, so the whole prize is **~0.012 Gi/f**. The 21.5 % killed by N.L and 10.7 % by the cone are per-pixel and no tiling reaches them. Two corroborations: a finer *raster* grid (`SESSION_STATE:3237-3262`) cost `gbuffer` **+139 % thrsum** — *"do not re-propose a uniform finer grid"*; and `--cone_fine_tiles` on city (6x4 -> 12x8) measured **no gain**. |
| **GGX hoists** | `BACKLOG:4123-4128` | Read the disassembly: **clang's LICM already hoists both** `Gv` and `4*NdotV` out of the depth-3 loop. Moot here anyway — no GGX lobe in OuterVec (§0.2). |
| **scanline carry** | `BACKLOG:3930-3952` | Three implementations, all net zero; OFF arms **+8.2 % / +20 % / +23 %**. The durable law: *"in this kernel a flag-guarded predicate or eight extra live values in the pixel body cost about what any of these mechanisms save."* (Note: measured in `TileFill`, not in OuterVec — `BACKLOG:557-559` misattributes it. The register-pressure argument generalises; the attribution does not.) |
| **slopeFn restructure** | `BACKLOG:551-561` | Ceiling for **any** rewrite of OuterVec's lane walk, measured by zeroing the slope: **-0.38 % of frame at t=1961**, -0.57 % at t=400. 16e already took 3.6x of this function's honest vector headroom, so an 8-wide batched form collects ~0.011 Gi/f = 0.26 % *before* paying the restructure. **"Predicted net: negative. Not built."** |
| **SoA Phase 5** | `SOA_VERTEX_REFACTOR.md:5-215` | Predicted 1.25 % of frame; **measured 0.56 %**, and no half pays (dense-write-only **+16 %**, shared-read-only **+33 %**; only the pair gives -28.6 %). The real lever is the 209.6 MiB per-light shadow clone, not `sizeof(Vertex)`; main view is neutral. Scope counted: **274 migrating derefs in DEMO scene code** (CITY 128), three whole transform pipelines. |
| **literal PCF split** | `BACKLOG:2794-2900` | Built. **Regresses at 5/5 poses** (+0.51 to +1.54 % Gi) because the split reaches a leaf by *exporting* the rare half into a 285-instruction function with nine callee-save pairs and its own 160 B frame. The premise was also wrong twice over: it targeted a frame carried by 0.43 % of taps, and the frame's actual cause was one lazy-static guard call, fixed separately by 16m. |
| **lane-level composite punting** | `BACKLOG:475-500` | Census before building: **94.2 % of a punted group's lanes genuinely need the scalar path** and 87.7 % of punted groups have all eight lanes reflective. A punted group is not a boundary artefact, it is the water region. Recovers **<=6 %** of the punt. |
| **live-water slope indirection** | `BACKLOG:520-566` | Devirtualising the `slopeFn` call inside *this kernel* is **-0.42 % of `lighting-w1`, -0.10 % of frame**; 94 483 calls/frame at 0.0040 Gi/f. Below bar, and the only way to collect it is a layering violation (FDS naming a DEMO symbol). It also corrected the record: of `--env_live_water`'s +0.041 Gi/f the slope is 0.015 and the mask read + weight + plane-hit + re-projection is 0.026. |
| **glow `atanf`** | `BACKLOG:418-473` | Not tableable (609 214 distinct args/frame), not hoistable (invariant at no loop level), reorder refuted by census (only 0.4 % wasted, 96.5 % contribute so deferring costs an extra atan per run). Ceiling from deleting it outright is **-0.79 % of frame**; the realistic polynomial collects **0.24 %**, under the 0.3 % bar. Not in this kernel anyway. |

**Two additional prohibitions that bear directly on this row:**

* **"The cube tap only gets cheaper by being CALLED LESS"** (`BACKLOG:4766-4783`): adding a
  provably-never-taken `bool` test to the tap cost **+12.4 % of `lighting-w1`**; the census
  hooks themselves cost **+2.0 %**; a runtime hatch bool in 16l cost **+4.3 % with the flag
  OFF**. *Any new runtime predicate inside a hot inner body must be priced against its own
  register-allocation cost, and the fix when it fails is to lift the predicate out of the
  loop entirely (16l used a template; 16h and 16o shipped flagless).*
* **`--texture_filter` is not a perf lever** (`BACKLOG:6212-6221`): deleting the albedo
  gather outright changes the kernel by **+0.9 %**. Do not propose making it default.
* **The `Material` hot-record is parked with a bound** (`BACKLOG:6444-6450`, `:5693`):
  measured at greets t=5743, 1 657 instructions and 440 cycles per pixel, **IPC 3.77**;
  bound on the win **<=0.1 ms/frame**. That refutation is a *latency* argument on the
  *scalar* kernel, which resolves the material **once** per pixel. It does not price the
  redundant *instructions* OuterVec spends resolving the same material up to 3x per lane
  and up to 8x per group. C3 below is scoped to the instruction half only, and says so.

---

## 5. Ranked candidates

Ranked by (predicted win) x (confidence) / (risk). `% of renderFrame = % of row x 0.229`.
The campaign bar is **0.3 % of `renderFrame`** (`BACKLOG:471`).

**Instrument hygiene.** There is **no ablation ladder for OuterVec** — `FDS_OMNI_ABLATE`,
`FDS_PIX_ABLATE`, `FDS_W2_ABLATE` and `FDS_W1LDR_ABLATE` (`:1682-1800`) are all inside
`TileT`/`TileFill`. Every candidate below therefore needs one built first, and it must be
**compile-time** (`-DFDS_OVEC_ABLATE=n`, `if constexpr`, cumulative sink) exactly like the
existing four, because a runtime-flag ladder in this body is the `+4.3 %`-with-the-flag-OFF
failure 16l documented. Counters must be `noinline` reporters with no loop-body state.

---

### **C1 — Publish `sEnvVecDiagOff` (and the three per-group flag reads) out of the pixel loop.** ← **BUILD THIS FIRST**

**(a) The change.** `DeferredSurfaceKernel.cpp:6342` declares a function-local
`static const bool sEnvVecDiagOff = !std::getenv("ENVPROBE") && !std::getenv("ENVFLIP") &&
!std::getenv("ENV_NOFETCH") && !std::getenv("FDS_ENV_SKIP_NEGY") && !EnvTraceGet();`
**inside the `for (px += 8)` loop.** Move it to file scope beside `g_envVecStats`
(`:965`), which is already exactly this shape. In the same edit, hoist to the kernel head
the three per-group `FeatureFlags` reads at `:6355-6358` (`env_sphere_parallax`, `env_ssr`,
and `envBrdfAnalyticG` is already hoisted), the `prof_no_lights()` read at `:6220`, and
gate the 8-iteration env-uniformity scan at `:6363-6369` on
`!_mm256_testz_si256(hasEnvV, hasEnvV)` using the `lane_hasEnv[]` array already built at
`:6034`. ~25 lines. **Flagless** — 16m, 16h and 16o all shipped this class flagless
precisely because a dial costs more than the mechanism.

**(b) Mechanism.** Two independent effects. The small one is instruction count: ~39-55
NEON instructions per 8-pixel group of pure re-derivation of loop-invariants (guard
acquire-load ~3, three flag reads ~9, uniformity scan ~24, `prof_no_lights` ~3). The large
one is **16m's mechanism verbatim**: a function-local `static` with a dynamic initializer
forces `bl __cxa_guard_acquire` into the function, and 16m measured that one such call —
*"the only call in 410 instructions"* — pinned eight callee-save pairs and a 144-byte frame
across the hot path, worth **-1.17 to -2.85 % Gcyc/f on `lighting-w1` at 5/5 greets poses**
for a ~30-line bit-exact change. This kernel is bigger and more register-starved than the
cube tap was.

**(c) Prediction.** Instruction half: ~47 instr/group / 8 = ~5.9 per pixel against ~530 =
**~1.1 % of the row = 0.25 % of `renderFrame`** — just under bar on its own. Register half:
if 16m's -1.17 to -2.85 % Gcyc reproduces, **1.2-2.9 % of the row on cycles = 0.27-0.66 %
of `renderFrame`**. Combined expectation **1.5-4 % of the row**.

**(d) Pixel-change risk.** **Byte-neutral by construction.** The environment variables do
not change during a run, so a static-init read and a first-use read return the same value;
the flag hoists move reads of a value that cannot change mid-frame; the `testz` gate skips
a scan whose only output (`uni`) would have stayed `nullptr`. Zero look risk.

**(e) Falsifying instrument.** `otool -tvV` on the two binaries: count callee-save pairs
and frame size for `Render_DeferredLighting_Tile_OuterVec`, and confirm
`bl __cxa_guard_acquire` disappears — the same structural table 16m used
(`BACKLOG:2856-2880`). Then min-of-11 order-rotated interleaved, one pose per process,
`Ginstr/f` **and** `Gcyc/f` for `lighting-w1` at t=1961 / t=2400 / t=400, plus an md5 pin
(`SESSION_STATE.md:1946` names the acceptance-arm hashes). If the frame does not shrink in
the disassembly, the register half is refuted and only the ~1.1 % instruction half is left.

**Why first:** smallest diff, byte-null by construction, mechanism already measured on this
exact row in a sibling kernel, and it needs no ablation ladder to judge.

---

### **C2 — Delete the mirror lane from the omni loop (template the light loop, not the kernel).**

**(a) The change.** In city both operands of the per-light mirror compare are constant
zero (§0.3). Extract `:6222-6287` into
`template <bool kMirror> static inline void ovecOmniLoop8(...)` and dispatch on
`const bool kM = !gb.mirrorId.empty();` computed once at the kernel head. In the
`kMirror == false` instantiation, drop `lane_mirror_v`, the `_mm256_set1_epi32(tl.mirrorId[n])`
broadcast, the `cmpeq`, and one `and`; and skip the eight-store zero-fill at `:5892-5893`
plus the vec reload at `:6222`. Duplicate **only the loop** (~80 instructions), not the
4 800-instruction kernel — 16l's I-cache objection to the second `TileT` instantiation
applies at kernel scale, not at loop scale.

**(b) Mechanism.** Removes 3 `__m256` ops = ~6 NEON instructions from a body of ~76, on
every (light x group) pair, with the predicate **outside the loop entirely** — which is the
form 16h/16l/16m all say is the only one that pays. Also removes 16 instructions per group
(8 stores + reload) that exist purely to feed a dead compare.

**(c) Prediction.** 6/76 = **7.9 % of the omni loop**. With the loop at ~47 % of the row
(§3.3) that is **3.7 % of the row**, plus 16/8 = 2 instr/px = 0.4 % of the row.
**~4.1 % of the row = 0.94 % of `renderFrame`.** Well above bar. Fountain and crash get it
too (neither allocates `gb.mirrorId`).

**(d) Pixel-change risk.** **Byte-neutral by construction, and provably so**: the mask is
`cmpeq(0,0)` = all ones at every lane of every group, and `x & 0xFFFFFFFF == x`. The
guarded arm is selected on `gb.mirrorId.empty()`, the exact condition under which
`lane_mirrorId[]` is filled with zeros. Greets under `--deferred_outer_vec` takes the
`kMirror == true` arm and is untouched.

**(e) Falsifying instrument.** `-DFDS_OVEC_ABLATE=<stage>` ladder (build it as part of this
round) with a stage that cuts the light loop immediately after the mask conjunction, to
price the loop's share of the row first. Then the two-arm A/B. Byte gate: the city
acceptance-arm md5 at `SESSION_STATE.md:1946` plus the greets pins with
`--deferred_outer_vec` forced on, which is the only way to exercise the `true` arm.

---

### **C3 — One material resolve per 8-pixel group when the group is material-uniform.**

**(a) The change.** At `:5964` the per-lane gather resolves `matTable.data[matID]` and
walks `Material` for **each of 8 lanes**, then walks it **again** at `:6088` (`MatN`) and a
third time at `:6510`/`:6621` (`Mat_`). City's facades are large flat quads, so the eight
matIDs of a horizontal 8-pixel run are almost always equal. Add a uniformity test —
`_mm256_cmpeq_epi32(matIDv, broadcast(lane0))` + `_mm256_movemask` == 0xFF, ~4 instructions
— and on the uniform path resolve `Material*` and `texData` **once**, compute
`Lumin*255 + Diff*amb{B,G,R}`, the `Glossiness > 0 ? gloss : 32` select, `wantSpec`,
`isWater` and `envP` **once**, and feed them to the vec body as `_mm256_set1_ps` broadcasts
instead of 8 scratch stores + a vec load. The 8 texel gathers stay scalar (different UVs);
the tint multiply becomes vec. The non-uniform path is the current code verbatim.

**(b) Mechanism.** Instruction removal, not latency (see the §4 prohibition — the parked
`Material` hot-record was refuted as a *latency* item at IPC 3.77 on the *scalar* kernel,
which resolves once per pixel; this is the *instruction* half, which that measurement did
not price). ~55-65 instructions per alive lane today; the uniform path removes the 7
redundant `Material` walks, 7 redundant `Txtr->Mipmap[mip]` chases, 7x4 ambient FMAs, 7
gloss selects, 7 pairs of scratch stores per property, and replaces the 13-array scratch
round-trip with broadcasts. Estimated ~30 of ~60 instructions/lane on the uniform path.
This is 16h's `fetchTexelNb` hoist transposed from "the neighbour shares the centre's
material" to "the eight lanes share one material".

**(c) Prediction.** The gather is ~60/530 = **11.3 % of the row**. At an assumed 85 %
uniform-group rate: 0.85 x (30/60) x 11.3 = **4.8 % of the row = 1.1 % of `renderFrame`**.
The uniform rate is the load-bearing unknown — it is the one number to census first.

**(d) Pixel-change risk.** **Byte-neutral.** Every arithmetic operation is identical
(`float(tx & 0xFF) * Mat->TintB` is the same IEEE mul whether done scalar or in a lane;
`Lumin*255.0f + Diff*ambB_sc` is the same FMA-or-not sequence as long as the hoisted form
keeps `-ffp-contract` behaviour — check the disassembly, this is the one place contraction
could differ, and `PERF_STATE.md`'s water-slope round documents three contraction traps of
exactly this kind). The uniform predicate only selects between two paths computing the
same values.

**(e) Falsifying instrument.** First a `noinline` reporter (uncommitted) counting uniform
vs non-uniform groups over the three city poses — if it is under ~60 % the candidate is
dead. Then the `-DFDS_OVEC_ABLATE` stage that cuts at the end of the gather, to confirm the
11.3 % share. Then A/B on `Ginstr/f` + md5 pin. Watch the OFF-arm column: a mis-shaped
uniformity test is exactly the "+8 to +23 %" register failure of 16h.

---

### **C4 — Group-level early-out for a light that reaches no lane.**

**(a) The change.** Insert, after `omni_lane` is formed at `:6242-6244`:
`if (_mm256_testz_ps(omni_lane, omni_lane)) continue;` — skipping the `safe_len2` blend,
`rsqrt`, `dist`, falloff, `k`, the entire spot block, the `intensity` blend and the three
colour broadcasts + FMAs.

**(b) Mechanism.** The three rejects (N.L, range, `len2 > 0`) are **spatially correlated
across 8 horizontally adjacent pixels**: they usually share a surface and therefore a
normal, so `dot < 0` fails for all 8 or none; and 8 adjacent pixels are within a few units
of each other in view space, so the range test agrees too. The `--omni_census` per-*pair*
kill rates on greets (`BACKLOG:4708-4724`) are N.L **21.49 %**, range **8.36 %**,
mirrorId 4.79 % — but the per-*group* rate is what matters here and is higher than the
per-pair rate wherever the rejects are correlated. Precedent for the shape is in the kernel
already: `anyVecLane` at `:6217` does exactly this one level up.

**(c) Prediction.** The test costs ~4 NEON instructions (256-bit `testz` -> 2x `umaxv` +
`orr` + `cbz`); the skipped remainder is ~40 of the ~76. At an assumed 30 % group-skip rate:
0.30 x (40/76) - (0.70 x 4/76) = **12.1 % of the omni loop = 5.7 % of the row = 1.3 % of
`renderFrame`**. At 15 % skip it is 2.4 % of the row; at 50 % it is 10.4 %. **The skip rate
is the whole candidate** and it is cheap to census.

**(d) Pixel-change risk.** **Byte-neutral by construction.** Every downstream term is
already multiplied by `omni_lane` via `_mm256_blendv_ps` at `:6283-6285`, so `intensity`
is exactly `+0.0f` in every lane, and `fmadd(+0.0f, colB, lB) == lB` bit-for-bit for finite
non-negative `lB` (which it is — it starts at `lane_ambB >= 0` and accumulates
non-negatives). Masked-off lanes carry `safe_len2 = 1.0f` so nothing goes non-finite.

**(e) Falsifying instrument.** A `noinline` reporter counting `(group x light)` pairs with
`testz(omni_lane)` true, over the three city poses — this number alone decides the
candidate, before any build. Then `Ginstr/f` **and `Gcyc/f`** A/B: a branch this
data-dependent can retire fewer instructions and cost more cycles through mispredicts,
which is precisely the 16l t=3409 pattern in reverse. Byte gate: city md5 pin.

---

### **C5 — Hoist the spot cone's reciprocal and skip the cone block when no lane is inside.**

**(a) The change.** `:6273-6274` computes `_mm256_div_ps(1.0f, cosInner[n] - cosOuter[n])`
inside the innermost loop, for a quantity constant per light. Add
`float rConeRange[DEFERRED_MAX_LIGHTS]` to `TileLights` (`DeferredCommon.h:196`), fill it
in `buildTileLightLists` (`DeferredLightLists.cpp:399`, `:503`) as
`1.0f / (Lci - Lco)`, and broadcast it. Additionally, wrap the cone block in
`if (_mm256_testz_ps(maskInside, maskInside)) continue;` — a spot whose cone misses all 8
lanes contributes exactly zero.

**(b) Mechanism.** A 256-bit float divide is 2x `fdivq_f32`: ~10-14 cycle latency each,
poorly pipelined on Apple silicon, and it sits on the critical path to `t` -> `smooth` ->
`coneAtten` -> `k` -> `intensity`. Moving it to list-build time turns
`(groups/tile) x (spots/tile)` divides into `spots/tile` divides — a **~1 700x reduction**
at 1512x848. City is spot-dense: `Render_VolumetricCones_Tile` is **row 1 of the profile at
20.6 % self time / 1.288 Gi/f**, and that pass exists only because spots do.

**(c) Prediction.** Instruction-wise this is small — 2 `fdiv` of ~76 per (spot x group), so
if S of the ~26 tile lights are spots the saving is `2S/(26x76) = 0.1 % x S` of the omni
loop; at S=6 that is **0.6 % of the omni loop = 0.3 % of the row**, i.e. **under bar on
`Ginstr/f`**. Cycle-wise it should read much larger because the divide is a latency, not a
throughput, cost — expect the gap between the `Ginstr/f` and `Gcyc/f` columns to be the
whole result, exactly as in 16m (Gi -0.96 to 0.00 %, Gcyc -1.17 to -2.85 %). The `testz`
half adds the same shape as C4 restricted to spots. **Ranked here, not higher, because I
cannot bound S statically and the instruction column probably says "noise".**

**(d) Pixel-change risk.** **Byte-neutral.** `1.0f/(a-b)` computed scalar at list-build and
broadcast is bit-identical to `_mm256_div_ps(set1(1.0f), sub(set1(a), set1(b)))` — both are
the correctly-rounded IEEE-754 single-precision subtract then divide. The `testz` skip is
byte-null by the same argument as C4 (`coneAtten` all-zero -> `k` all-zero -> `intensity`
all-zero). **Do not** substitute a reciprocal *estimate* for the divide; that would move
bytes.

**(e) Falsifying instrument.** Census the tile lists for `isSpot` count per tile (a
`noinline` reporter in `buildTileLightLists`) — if city's tile lists carry <3 spots this is
below bar and should not be built. Then `Gcyc/f` A/B, with `Ginstr/f` reported as the
control that is *expected* to be flat.

---

### **C6 — Give the `wantSpec` redo lane a specular-only loop instead of a full re-shade. (LARGEST — and blocked on one measurement and one look call.)**

**(a) The change.** A lane with `Specular > 0` runs the vec light loop at `:6222` (which
accumulates its diffuse into `lB/lG/lR` — `omniMask` at `:6202` masks off water but **not**
`wantSpec`) and then runs a **second complete** `for n < tl.count` loop at `:6428-6469`
that recomputes N.L, range, `fast_rsqrt`, falloff and the diffuse accumulate all over
again, purely to reach the specular half. `vfB/vfG/vfR[k]` — the vec result — is discarded
at `:6547`. The change: take the diffuse from the vec pass and let the redo compute only
the half-vector and `std::pow`.

**(b) Mechanism.** The redo loop is ~22 scalar instructions per (spec-pixel x light) for
the diffuse skeleton plus ~12 for the specular. Deleting the skeleton is a ~64 % cut of the
most expensive per-pixel path in the kernel — and it is *pure duplication*, the single
clearest structural waste in the row.

**(c) Prediction.** At 26 lights the redo costs ~570 instructions per spec pixel against a
~530 whole-pixel average, so a spec pixel is roughly **2x** a matte one. If spec pixels are
a fraction *f* of shaded pixels, the redo is `f x 570 / (530 + f x ...)` of the row; at
*f* = 0.30 the redo is ~28 % of the row and cutting 64 % of it is **~18 % of the row = 4.1 %
of `renderFrame`**. At *f* = 0.10 it is ~7 % of the row. **This is the biggest item on the
list by a factor of three and I cannot size it without *f*.**

**(d) Pixel-change risk. NOT byte-neutral as stated — this is the catch.** The vec loop's
`lenInv` is a bare `_mm256_rsqrt_ps` (`:6248`, `vrsqrteq`, no Newton) and the scalar redo's
is `fast_rsqrt` (`SimdHelpers.h:16`, `vrsqrte` + **one** Newton step). The kernel's own
comment at `:6043-6053` puts the bare estimate at **+-0.3 % and piecewise-constant in its
LUT**; the refined form is +-6e-5. Taking the diffuse from the vec pass therefore moves the
diffuse of every spec pixel by up to **+-0.3 %, i.e. up to +-0.8 of an 8-bit level**, with
LUT-cell stepping. So the change decomposes into two:
  * **C6a (a correctness question, not a perf one):** unify the two reciprocal square roots.
    Adding one Newton step to the vec loop costs 3 `__m256` ops = ~6 NEON instructions of
    ~76 (**+7.9 % of the omni loop**) and makes the vec diffuse *more* accurate — but it
    **changes every city pixel**, and it is exactly the defect `--vec_ggx_refine`
    (`FeatureFlags.def:663`) documents one loop over: *"the SAME scene shaded through
    `--deferred_vec` produced a DIFFERENT IMAGE on the two architectures"*. This is a look
    call for Gil-Ad, logged in `SHADING_CONTRACT.md`, not an optimisation.
  * **C6b:** with C6a landed either way, the redo split is byte-null.
  Note the perverse arithmetic: C6a **costs** ~7.9 % of the omni loop (~3.7 % of the row) to
  make C6b's ~18 % byte-null. Net still strongly positive if *f* is large.

**(e) Falsifying instrument.** In order: (1) `--deferred_gloss_stats` (already committed,
`:7656`) for the material count; (2) an uncommitted `noinline` reporter on
`lane_needs_scalar[k]` at `:6399` for *f* at t=1961 / t=2400 / t=400 — **if *f* < 0.08 this
candidate is below bar and should be closed**; (3) a five-pose pixel diff of the C6a arm
against the pin, reported as "px moved / mean |dY| / max |dY|" in the shape
`--vec_ggx_refine`'s own text uses, for the look call.

---

### **C7 — Vector pack of the plain-lane store.**

**(a) The change.** `:6646-6665` converts, clamps and stores **eight separate dwords**. When
no lane in the group needs the scalar redo or the env compose — the common case on city
facades and road — do it 8-wide: `_mm256_cvttps_epi32` x3, `min`/`max` clamps x6,
`slli`+`or` x4, one `_mm256_storeu_si256`.

**(b) Mechanism.** ~14 scalar instructions per lane (3 float->int converts, 6 compare-and-
select clamps, 2 shifts, 2 ors, 1 store) = ~112 per group, against ~30 for the vec form.

**(c) Prediction.** ~10 instructions/px of ~530 = **1.9 % of the row = 0.43 % of
`renderFrame`**, times the all-plain-group rate (high in city, but the same census as C3
answers it).

**(d) Pixel-change risk.** **Byte-neutral under this arm, with one guard.** `int(f)` is
truncation toward zero, which is exactly `_mm256_cvttps_epi32`. The one divergence is
out-of-range input: `cvttps` returns `INT_MIN` for values above `2^31`, which after the
`>255 -> 255` clamp would become 0 instead of 255. Under `--no-hdr` (his arm) `lB <= 250`
and `texB <= 255*Tint`, so `fdB <= ~249` and the case is unreachable — but the vec form
must clamp **in float before the convert**, not after, so the guard is unconditional and
the code is correct under `--hdr` too.

**(e) Falsifying instrument.** `-DFDS_OVEC_ABLATE` stage cutting after the compose but
before the pack, to price it; then A/B + md5 pin, plus a deliberate `--hdr` run to prove
the clamp ordering.

---

### **C8 — Skip the dead normal-map lane loop when no material in the pass carries one.**

**(a) The change.** `:6086-6147` stores `nx/ny/nz` to `nx_lane[]`, runs an 8-iteration
per-lane loop that re-derives matID, re-resolves `Material*` and tests `MatN->NormalMap`,
then reloads `nx/ny/nz`. When no material in `ctx.matTable` has a `NormalMap` (city's
buildings do not), the loop provably does nothing and the store/reload is the identity.
Compute `anyNormalMap` once per pass, and skip the loop **and** the round-trip.

**(b) Mechanism.** ~8 instructions per lane of dead work (64/group) plus 6 vec stores + 6
vec loads for the round-trip (~24/group) = ~88/group = **11 instructions/px**.

**(c) Prediction.** 11/530 = **2.1 % of the row = 0.48 % of `renderFrame`**. Contingent on
city genuinely having no normal-mapped material — verify with a scan of `matTable`, not by
assumption.

**(d) Pixel-change risk.** **Byte-neutral by construction** if the predicate is
`no material in matTable has NormalMap`: the loop's only writes are `nx_lane[k] = ...`
inside `if (MatN->NormalMap)`, and the reload of an unmodified buffer is the identity.

**(e) Falsifying instrument.** A one-line scan printing the count of materials with
`NormalMap != nullptr` per scene (fold into `--deferred_gloss_stats`'s existing dump rather
than adding a flag). Then A/B + pin. Note this is one of the cheapest items and it should
be *combined* with C1 into a single "dead work above the light loop" commit, since both are
byte-null by construction and neither needs a ladder to judge.

---

### Summary table

| # | candidate | predicted % of row | % of `renderFrame` | bytes | confidence |
|---|---|--:|--:|---|---|
| **C1** | publish `sEnvVecDiagOff` + hoist per-group flag reads | 1.5-4 % | 0.34-0.9 % | **null by construction** | **high** (16m measured the mechanism on this row) |
| C2 | template the mirror lane out of the light loop | 4.1 % | 0.94 % | **null by construction** | high (mask provably all-ones) |
| C3 | one material resolve per uniform group | 4.8 % | 1.1 % | null (check contraction) | medium — needs the uniform-group census |
| C4 | `testz(omni_lane)` group-level light skip | 2.4-10 % | 0.5-2.4 % | **null by construction** | medium — needs the skip-rate census |
| C5 | hoist the cone reciprocal + cone `testz` | ~0.3 % Gi, larger Gcyc | ~0.07 % Gi | null | medium-low — needs the spot count |
| **C6** | specular-only redo lane (+ C6a rsqrt unification) | **~18 %** at *f*=0.30 | **~4.1 %** | **C6a moves every pixel** | low on size, high on mechanism |
| C7 | 8-wide pack of the plain lanes | 1.9 % | 0.43 % | null (clamp before convert) | medium |
| C8 | skip the dead nmap lane loop | 2.1 % | 0.48 % | **null by construction** | high, if city has no normal maps |

**Build order.** C1 + C8 as one flagless commit (both byte-null by construction, ~40 lines,
no ladder needed, and C1 has a measured precedent on this row). Then build the
`-DFDS_OVEC_ABLATE` ladder and the three censuses (uniform-group rate, `omni_lane` skip
rate, `lane_needs_scalar` rate) in one instrumented binary — **uncommitted** — because
those three numbers decide C3, C4 and C6, which are 75 % of the total prize. Then C2.

**The one thing that would change this whole ranking:** *f*, the fraction of city's shaded
pixels whose material has `Specular > 0`. If it is large, C6 dwarfs everything else and
the round should be about the duplicated light loop. If it is small, the row is the
Lambertian omni accumulate and C2 + C4 are the round.
