# PERF_STATE.md — current state of the deferred pipeline (greets, 2026-05)

> **§0 below (2026-08-08) supersedes the numbers in §1–§2 and §9.** Those were
> estimates and partial brackets from 2026-05, before `renderFrame` had any interior
> instrumentation and before the PBR/mip/env defaults moved. §0 is a measured,
> self-checking phase split of five poses across three scenes. The rest of the
> document remains the best description of the *mechanism* (kernel structure, cube
> tap, sampling modes, flag inventory) — read it for "how", read §0 for "how much".

State of the engine on `feature/static-shadow-lightmaps`, gathered for invasive perf work on
the deferred kernel. Numbers measured at greets `t=500`, 1920×1080, low-poly Piramid (5.5k
faces), chunked at `--greets-piramid-chunk-grid=8`, per-cube-face cull on. Read this top-to-
bottom once; thereafter use the tables.

This is descriptive, not prescriptive. Things that look wasteful are flagged; fixes are not
proposed here.

---

## 0. MEASURED phase split of `renderFrame` — 2026-08-08

Instrument: `--deferred_prof=<warmup>` (`FDS/RENDER/TailProf.h`, see
`docs/GRAPHICS_PIPELINE.md` §8b). 1920×1080, 12 pool workers, scene defaults as
shipped (greets: PBR + HDR + bloom 2.0 + checkerboard + mirror + `cone_fine_tiles`;
city: `cine::kCity`; fountain: `cine::kFountain`).
`--bench=scene@scene=<s>,t=<T>,iters=60`, warmup 5, **min over steady frames, then
min-of-arm over 3 interleaved repetitions.**

**Load: the machine was shared for the whole campaign** (two other agents running
`./DEMO`; 1-min load average 16–57 per run, recorded per row). Serial phases
(`hdr-begin`, `mirror-grid`, `depth-bounds`, `tile-cull`) came out identical to 3
decimals across every arm and act as the internal control; the *parallel* waves are
the load-sensitive ones, which is why every comparison below is interleaved and
min-of-arm. **Sub-0.1 ms phases are not resolvable here and are not interpreted.**

`OTHER` (per-frame `renderFrame` minus Σ of its phases) is **0.048–0.056 ms on every
greets/fountain row and 0.15 ms on city** — i.e. the frame is fully attributed, which
is what makes the rest of the table quotable.

### The five poses (ms/frame, `wall_min`)

| phase | greets 5743 | greets 2000 | greets 4200 | fountain 2500 | city 1961 |
|---|--:|--:|--:|--:|--:|
| **renderFrame (= essentially all of RNDR)** | **43.65** | **63.82** | **38.81** | **20.09** | **69.82** |
| G-buffer clear | 0.34 | 0.32 | 0.32 | 0.34 | 0.73 |
| **G-buffer fill (`gbuffer`)** | **5.59** | **4.77** | **5.12** | **2.51** | **9.20** |
| HDR buffer begin (33 MB f32 clear) | 0.13 | 0.13 | 0.13 | — | — |
| **deferred lighting (total)** | **32.59** | **47.14** | **29.18** | **1.74** | **10.48** |
| ⤷ light list + per-tile cull + depth bounds + mirror grid | 1.04 | 1.07 | 1.03 | 0.29 | 0.74 |
| ⤷ shading wave 1 (`lighting-w1`) | 27.32 | 35.57 | 24.25 | 1.36 | 9.38 |
| ⤷ checkerboard fill wave 2 | 3.14 | 9.20 | 3.04 | — | — |
| froxel fog (`fastfog` + sky paint) | — | — | — | — | 10.39 |
| volumetric cones (`cones-call`) | 1.22 | 6.20 | 0.53 | — | **30.72** |
| transparent peel (`xpar-peel`) | 0.08 | 0.05 | 0.05 | 0.14 | 0.25 |
| **TBR (sprites + unified transparents)** | 0.59 | 2.19 | 0.21 | **14.77** | 7.24 |
| bloom chain (DoF+bright+anam+bloom+ghosts) | 1.74 | 1.78 | 1.77 | — | — |
| tonemap + LDR post | 0.69 | 0.67 | 0.69 | — | — |
| everything else (sprite insert, prologue, overlays, edge AA) | <0.03 | <0.03 | <0.03 | 0.16 | 0.06 |
| `OTHER` (unattributed) | 0.049 | 0.051 | 0.049 | 0.053 | 0.149 |
| — outside RNDR — | | | | | |
| shadow cube bake (`BAKE` section) | 2.05 | 3.39 | 2.28 | 0 | 0 |

Notes on shape:
- city's `renderFrame` runs **twice per frame** (the water-reflection underlay plus
  the final view); the column is the per-frame total of both.
- **Mirror RTT and the shadow bake are NOT inside RNDR.** In `GREETS.CPP` the RTT sits
  in `PROF_ANIM` (~L3712) and `ShadowBake_DispatchGreets` in `PROF_BAKE` (~L3813); both
  measured 0.00 and 2.05 ms respectively at t=5743. Nothing hides in RNDR on their behalf.
- fountain is the only scene with a TBR, and it is **73 % of its frame**.

### Wall vs thread-sum (they answer different questions)

`wall` above is ELAPSED on the tick thread and sums to the frame. The parallel waves
also report `thrsum` = Σ tile-task durations (CORE-ms) and `effPar = thrsum/wall`:

| wave (greets 5743) | wall | thrsum | effPar (of 12) |
|---|--:|--:|--:|
| `lighting-w1` | 27.3 | ~370–520 | **10.2–11.4** |
| `lighting-w2` | 3.1 | ~40–49 | 5.9–11.3 |
| `gbuffer` | 5.6 | ~62–76 | 6.7–9.7 |
| `cones` | 1.2 | ~9–13 | 4.1–7.4 |

**The lighting wave is compute-bound and balanced, not barrier-tail-bound** (effPar
10–11 of 12 workers). There is no reclaimable idle there: the fix has to remove
per-pixel work, not rebalance tiles. The `gbuffer` and `cones` waves *do* leave
parallelism on the table (effPar 4–10), and `cones` runs on the coarse 6×4 grid
outside greets.

### Inside the lighting wave — ablation

`--prof_no_lights` (omni loop off) against the same-pose baseline, control phases
matched within 2–9 %:

| pose | `lighting-w1` base | `--prof_no_lights` | ⇒ omni loop | share of frame |
|---|--:|--:|--:|--:|
| greets 5743 | 27.32 | 8.28 | **19.0** | **44 %** |
| greets 2000 | 35.57 | 10.14 | **25.4** | **40 %** |
| city 1961 | 9.38 | 5.99 | 3.4 | 5 % |

So on greets the **per-light loop (cube-shadow taps + per-light PBR) is the single
largest slice of the frame**, and the remaining ~8 ms of the wave is the G-buffer
decode + material resolve + all the map fetches + ambient/SH + env compose.

Three independent sets at greets 5743 (this one, plus the mechanism and shadow sets
below, taken at different loads on different arms) put the omni loop at **19.0 / 21.25
/ 20.61 ms** — call it **~20 ms, 43–46 % of the frame**. The spread is entirely in the
baseline `lighting-w1` (27.32 / 29.50 / 28.90, load-driven); `--prof_no_lights` itself
lands at **8.28 / 8.26 / 8.29 ms across all three**, which is as tight a repeat as this
machine gives and is why the non-light remainder is quoted with confidence.

#### Splitting the omni loop — shadow sampling vs per-light shading

Tightly interleaved, greets t=5743, **3 reps each at load 22–26** (the quietest set of
the campaign; controls `hdr-begin` 0.128–0.129, `mirror-grid` 0.645–0.661,
`bloom-chain` 1.82–1.89, `gbuffer` 5.52–5.80 — all matched within 4 %):

| arm | `renderFrame` | `lighting-w1` | removes |
|---|--:|--:|---|
| base | 46.34 | **28.90** | — |
| `--shadow_polyid_no_pcf` | 44.59 | **27.10** | 3 of the 4 PCF taps |
| `--no-shadows` | 35.09 | **18.10** | all shadow sampling |
| `--prof_no_lights` | 25.85 | **8.29** | the whole omni loop |

Differencing the shading wave:

| component | ms | % of `lighting-w1` | % of the 46.3 ms frame |
|---|--:|--:|--:|
| everything before/around the light loop (G-buffer decode, matID→Material\*, normal/metal/rough/AO/horizon fetches, ambient + SH, env compose, store) | **8.29** | 29 % | 18 % |
| per-light shading math, shadows excluded (range/cone tests, attenuation, GGX/Fresnel, accumulate) | **9.81** | 34 % | 21 % |
| shadow sampling (cube taps + lightmap + spot maps) | **10.80** | 37 % | **23 %** |
| ⤷ of which the 3 extra PCF taps | 1.80 | 6 % | 4 % |

The 2026-05 estimate at the top of this document put "per-pixel cube-shadow taps" at
~32 ms and called it the #1 cost; on today's content and defaults it is **10.8 ms** and
it is roughly TIED with the per-light shading math it sits inside. The headline has
changed: no single stage owns the greets frame — shadows 23 %, per-light math 21 %,
the rest of the kernel 18 %, the checkerboard fill wave 7 %, the G-buffer fill 12 %.

### `--texture_filter` 0 / 1 / 2 — MEASURED, and it does NOT pay

Hypothesis under test: the G-buffer stores a texel ADDRESS, so the kernel pays a
dependent random gather per pixel; `--texture_filter>0` makes the rasterizer write a
filtered BGRA plane the kernel reads LINEARLY, which might be a free perf win.

`renderFrame` ms/frame, min-of-arm over 3 interleaved reps:

| pose | tf=0 | tf=1 (bilinear) | tf=2 (trilinear) |
|---|--:|--:|--:|
| greets 5743 | **43.65** | 44.58 | 45.36 |
| greets 2000 | **63.82** | 65.85 | 65.87 |
| greets 4200 | **38.81** | 39.68 | 41.17 |
| fountain 2500 | 20.09 | **20.03** | 21.09 |
| city 1961 | **69.82** | 71.32 | 73.79 |

It **costs** 0.9–4.0 ms and never wins. The two phases that move say why:

| pose | `lighting-w1` tf0→tf1→tf2 | `gbuffer` tf0→tf1→tf2 |
|---|---|---|
| greets 5743 | 27.32 → 27.29 → 27.53 | 5.59 → 6.76 → 6.86 |
| greets 2000 | 35.57 → 35.52 → 35.93 | 4.77 → 6.11 → 6.23 |
| greets 4200 | 24.25 → 23.99 → 25.03 | 5.12 → 6.52 → 6.71 |
| city 1961 | 9.38 → 9.39 → 9.59 | 9.20 → 10.06 → 10.95 |
| fountain 2500 | 1.36 → 1.32 → 1.39 | 2.51 → 2.84 → 3.07 |

**The kernel does not get faster — at all, at any pose.** The mechanism: the filtered
plane replaces only the *albedo* gather. The normal, metal, roughness, AO and horizon
maps are still fetched at the same `Mipmap[miplevel][swizzledUV]` address
(`DeferredSurfaceKernel.cpp` ~1659/2519/2535/1905/1972), so the dependent address
chase and its miss pattern survive intact — one of five gathers removed changes
nothing measurable. Meanwhile the raster pass pays a consistent **+1.2–1.4 ms** to
bilinear-sample and write an extra full-res BGRA plane.

`--texture_filter` remains a QUALITY flag (it fixes texel crawl). It is not a perf
lever, and it should not be defaulted on for performance reasons.

#### The direct proof: `--prof_no_tex` makes the kernel no faster

A separate, TIGHTLY interleaved set at greets t=5743 (every arm back-to-back inside
each rep so they share the same competing load; 4 reps; control phases `hdr-begin`
0.129-0.130, `cones-call` 1.218-1.274, `TBR-render` 0.619-0.650 confirm comparability):

| arm | `renderFrame` | `lighting-w1` | `gbuffer` |
|---|--:|--:|--:|
| baseline | 46.42 | **29.50** | 5.93 |
| `--prof_no_tex` (albedo fetch -> constant) | 47.47 | **28.79** | 5.87 |
| `--texture_filter=1` | 49.13 | **29.46** | 7.30 |
| `--texture_filter=1 --prof_no_tex` | 47.51 | 28.92 | 7.23 |
| `--prof_no_spec` | 44.42 | 26.43 | 5.50 |
| `--prof_no_lights` | 25.53 | **8.26** | 5.59 |

Two rows kill candidate (C) between them:

- **The albedo gather is worth only 0.71 ms** (29.50 - 28.79 = 2.4 % of the wave,
  1.5 % of the frame). Deleting it *outright* - not replacing it, deleting it - buys
  0.7 ms. That is the entire prize the filtered-albedo plane is competing for.
- **The filtered plane does not even collect it: 29.46 vs 29.50, a 0.14 % difference**,
  while `gbuffer` pays +1.37 ms for the extra plane. Net loss by construction.

The reason is structural, not incidental: the plane replaces one of *five* fetches at
the same `Mipmap[miplevel][swizzledUV]` address - normal, metal, roughness, AO and
horizon maps all still chase it (`DeferredSurfaceKernel.cpp` ~1659 / 2519 / 2535 /
1905 / 1972). Removing one of five leaves the address chase and its miss pattern
intact.

Same set, other splits: **specular = 3.07 ms** (29.50 - 26.43, 10 % of the wave);
**omni loop = 21.25 ms** (29.50 - 8.26, **72 % of the wave, 46 % of the frame**),
which agrees with the independent 20.61 ms from the shadow set above (different arms,
different load) and whose non-light remainder agrees to 8.26 vs 8.29.

### Anchor — the `RNDR 64.017` in GPU_BENCHMARK_PLAN §6.2c, split

§6.2c's CPU column was taken at greets t=5743 under the matched-tier flags
(`--no-greets_mirror --no-mirror_rtt --no-greets_disco --no-parallax
--no-shadow_lightmap --no-deferred_checkerboard`). Re-run today on this tree
alongside the shipped-defaults arm, 3 interleaved reps:

| | shipped defaults | §6.2c tier C | §6.2c reported |
|---|--:|--:|--:|
| frame min | 50.59 | **66.67** | 67.61 |
| `RNDR` min | 46.72 | **62.93** | 64.017 |
| `BAKE` min | 3.06 | 3.04 | 2.992 |
| `renderFrame` | 46.10 | 62.80 | — |
| ⤷ `lighting-w1` | 28.83 | **56.22** | — |
| ⤷ `gbuffer` | 5.69 | 2.30 | — |
| ⤷ `lighting-w2` (checkerboard fill) | 3.35 | n/a (full rate) | — |

So §6.2c reproduces within **1.7 %**, and its 64 ms is **89.5 % one thing: the
deferred lighting shading wave** at full shading rate. (The tier's `--no-greets_mirror`
also removes the mirror clone geometry, which is why its G-buffer fill is 2.30 vs 5.69
— the clone costs ~3.4 ms of raster in the shipped configuration.)

### Per-tile light census (`FDS_TILE_LIGHT_PROF=1`), 12×8 grid = 160×135 px

| pose | view lights | avg/tile | avg/non-empty tile | max |
|---|--:|--:|--:|--:|
| greets 5743 | 117 | 6.9 | 8.3 | 39 |
| greets 2000 | 117 | 7.3 | 8.8 | 41 |
| city 1961 | 76 | 26.1 | 33.0 | 53 |

**Read this next to the omni-loop table above and it settles an argument:** city
carries **3.8× more lights per tile than greets** and its omni loop costs **3.4 ms**;
greets carries 6.9 and its omni loop costs **19–21 ms**. Whatever sets the per-light
cost, it is **not the per-tile light count** — the two move in opposite directions by
a factor of ~23. Any proposal that attacks lights-per-pixel has to get past this row
first.

I did **not** determine why the per-light cost differs so much between the two
scenes, and will not guess: the obvious candidate (greets on the scalar path, city on
the 8-wide vec path) is **wrong** — `deferred_vec` defaults OFF on arm64
(`FDS_DEFERRED_VEC_DEFAULT`, FeatureFlags.h), so both scenes run the scalar per-light
loop on this machine. Plausible remaining differences (unmeasured): how many of each
tile's lights survive the per-pixel range/cone test, how many pixels are sky
(`zEnc == 0`, skipped entirely), and how many lights carry a cube shadow map. **The
per-pixel surviving-light count is the measurement candidate (B) actually needs and
it does not exist yet** — the census above counts per TILE, not per pixel.

### `--cone_fine_tiles` on city — measured, no win

The cone pass is 30.7 ms = 44 % of the city frame and runs on the coarse 6×4 grid
there (greets defaults it to 12×8, where it was worth ~8 %). Interleaved A/B at
city t=1961, 3 reps each, load 28–37 — an unusually stable pair (`renderFrame` spread
0.8 % within each arm):

| | coarse 6×4 | fine 12×8 |
|---|--:|--:|
| `cones-call` | 30.866 | 30.967 |
| `renderFrame` | 70.837 | 71.399 |
| `RNDR` min | 79.763 | 79.136 |

**No gain — the greets result does not transfer.** The two arms are within 0.8 %,
i.e. inside the run-to-run spread. city's cone cost is not a tile-balance problem.

---

## 1. Pipeline overview — one greets frame in deferred mode

| # | Stage | File | Function | Threading | ms (greets t=500, 1080p, full shadows) |
|--:|---|---|---|---|--:|
| 1 | Splines + animation | `FDS/RENDER/Transform.cpp` | `Animate_Objects` | single | ~0 |
| 2 | Per-frame greets driver (robot spot tracking, orbit spots) | `DEMO/GREETS.CPP` | inline in `Tick_Greets` (~L1732+) | single | ~0 |
| 3 | Main camera transform/cull | `FDS/RENDER/Transform.cpp` | `Transform_Objects(GreetSc, g_mainCamera, g_mainFaces)` | per-mesh thread tasks | ~few |
| 4 | Lighting (per-vertex, forward) | `FDS/RENDER/Lighting.cpp` | `Lighting(GreetSc)` | single | low — deferred path skips most of it |
| 5 | Radix sort of FList | `FDS/SORTS` | `Radix_Sort` | single | low |
| 6 | Main rasterize (G-buffer fill) | `FDS/RENDER/RENDER.CPP` L344-358 → `RenderInnerMekalele` → `Mekalele.h::Mekalele` | tiled 6×4 over threadpool | included in baseline-no-shadow |
| 7 | Dynamic-omni shadow bake | `GREETS.CPP:1850` → `FDS/RENDER/Shadows.cpp::Render_DeferredShadowMaps(_, DynamicOmnisPerFrame)` | per-light task (Phase A) + light×tile task (Phase B) | ~12.5 ms |
| 8 | Dynamic-mesh-into-static bake | `GREETS.CPP:1856` (gated on `--shadow-dynamic`) | as above, mode `DynamicMeshesPerFrame` | ~0 (off by default in measurements above) |
| 9 | Deferred lighting kernel | `FDS/RENDER/DeferredLighting.cpp::Render_DeferredLighting` (L3032) | tiled 12×8 over threadpool | ~44 ms (12 ms "other" + 32 ms cube tap) |
| 10 | Deferred skybox | same file, `Render_DeferredSkybox` | gated `--deferred-skybox` (off by default) | 0 |
| 11 | Deferred fog | `DeferredLighting.cpp::Render_DeferredFogPass` (L5411) | tiled 6×4 | ~small (Scn_Fogged) |
| 12 | Transparent peel (per mesh, 2-layer G-buffer) | `RENDER.CPP::renderFrame` L431-530 | per-mesh batches → tiled raster + tiled lighting | small for greets |
| 13 | Particles / sprites | inline in renderFrame L534-555 + `TBR_Render` | single + tile | low |
| 14 | Volumetric (cones + halos OR unified) | `DeferredLighting.cpp::Render_VolumetricCones`/`_OmniHalos`/`_DeferredVolumetric` | tiled | depends on flags |
| 15 | Lightmap viz overlay | `FDS/RENDER/LightmapBake.cpp::Render_LightmapViz` | single | 0 when off |
| 16 | Shadow-map debug overlay | `FDS/FILLERS/ShadowMap.cpp::ShadowMap_Overlay` | single | small |
| 17 | SDL flip | `DEMO/SDL2.cpp` | `MainSurf->Flip` → `SDL_UpdateTexture`+`SDL_RenderCopy`+`SDL_RenderPresent` | single | ~vsync |

Baseline-no-shadow = stages 1–6 + 9–17 with `prof_no_cube_tap` and `--no-shadows`. Stage 9 is
still ~12 ms even without any cube taps (see §2). The 33 ms baseline is roughly:

| Component of the 33 ms baseline | rough share |
|---|--:|
| Per-pixel deferred kernel "other" (texel fetch + ambient + omni loop + spec + sat + modulate + store) | ~12 ms |
| G-buffer fill (Mekalele rasterize over 5.5k Piramid + robot faces) | ~few ms |
| Main `Transform_Objects` + sort | low single-digit |
| Fog pass (Scn_Fogged) | low single-digit |
| Volumetric (cones/halos default on for greets) | a few ms — depends on cone_strength / halo_strength |
| Skybox/forward sprite + flip + present | rest |

The 1920×1080 framebuffer is 2.07 Mpx. Even an empty per-pixel pass that just stores 4 bytes
is ~3-4 ms at memory bandwidth, so ~12 ms of "kernel other" is plausibly:
texel gather (random reads into Material->Txtr->Mipmap[mip]) + ambient compute + Mekalele
mat32-decode + ZPage decode + (tiny) per-tile omni loop + spec for the lit pixels +
saturate/modulate/store. The texel-fetch cache miss pattern is the largest single piece.

## 2. The deferred lighting kernel

Entry: `Render_DeferredLighting()` at `DeferredLighting.cpp:3032`.

Per frame the entry does:
1. Build view-space omni list `ViewLightsSoA lights` (L3083-3174). Walks `Sc->OmniHead`,
   transforms each `Omni::IPos` into view space, copies color×ISize, range², 1/range, spot
   cone, world position, `shadowMapIdx`, `cubeShadowIdx`, halo params. Capped at
   `DEFERRED_MAX_LIGHTS=128`.
2. `computeTileDepthBounds` + `buildTileLightLists` populates 12×8 `TileLights` (L3192-3199).
   Each tile gets a compacted SoA of omnis whose screen bounding circle overlaps it.
3. (Optional) `buildStripLightLists` for unified-TBR transparent path.
4. Wave 1: 96 tile jobs into `Render_DeferredLighting_Tile` (L953) or
   `Render_DeferredLighting_Tile_OuterVec` (L2193), selected by `deferredLightingOuterVecEnabled()`.
5. Wave 2 (only if `checkerboard` or `quarter` on): 96 tile jobs into
   `Render_DeferredLighting_TileFill` (L2728).

### Two kernel implementations

| Aspect | Scalar `_Tile` (L953) | OuterVec `_Tile_OuterVec` (L2193) |
|---|---|---|
| Outer loop | scalar pixel-by-pixel | 8 px per row in `__m256` |
| Inner omni loop | scalar with branch-predicted early-out OR (when `deferred_vec=1`) 8 omnis-per-pixel vec body L1268-1399 | 1 omni × 8 pixels per iter L2510-2566 |
| Texel fetch / matTable / ambient | scalar | scalar per-lane (no vgather on arm64/simde) |
| Normal decode (oct → x/y/z) | scalar `meka::oct_decode_u16` | 8-wide via the inline code L2360-2391 |
| Normal-map (TBN) | scalar block L1108-1152 | per-lane scalar L2400-2457 — forces lanes through scalar fallback in the omni loop |
| Spec | per-omni `pow_glossClass` (template ladder by gloss) | spec lanes go through full **scalar fallback** L2612-2694 — the OuterVec omni loop is short-circuited for them |
| Water | special case `isWater` skip + half-blend | same lanes go through scalar fallback |
| 2D spot shadow tap | scalar inline at L1521-1640 | NOT in OuterVec vec body; only reached via scalar fallback (and the fallback inside L2612-2694 does not run the 2D shadow tap either — see §8 quirk) |
| Cube shadow tap (`resolveCubeAtten`) | scalar L1646-1657 (called once per omni per pixel) | scalarized lane-by-lane L1349-1378 *inside* the inner-vec body of the standard tile when `deferred_vec=1`, plus the OuterVec spec fallback omits cube tap entirely (see §8) |

`deferredLightingOuterVecEnabled()` (L737-744) tri-state:
- Explicit CLI/env `--deferred-outer-vec` wins.
- Else `CurScene->PreferOuterVec`.
- City / Crash / Fountain set it to 1 (matte/spec-light scenes win).
- Greets does NOT set it → defaults to 0 → greets runs the **scalar `_Tile`**.

This matches memory tag `project_outervec_incorrect`: OuterVec matches std visually (0.42%
pixel diff) but is ~2 ms slower on greets because most greets pixels are spec+nmap → scalar
fallback path. Off by default for greets.

### Per-pixel stages in scalar `_Tile` (the actual greets kernel)

Reading the code top-to-bottom in `Render_DeferredLighting_Tile`, every alive pixel goes
through (assuming default flags + matte path):

| Step | Lines | What happens | SIMD? |
|---|---|---|---|
| Wave-skip test | 998-1001 | early `continue` for odd cells if `checker`/`quarter` | scalar |
| Z decode | 1003-1006 | one load `ZPage16[i]`, branch on `zEnc==0` | scalar |
| mat32 decode | 1008-1011 | one load, 3 shifts | scalar |
| matTable bounds + Material* + Txtr null guards | 1012-1014 | one indirection | scalar |
| `surfaceShadowId` decode | 1023-1025 | one load `gb.shadowMatID[i]` (16b) or fallback | scalar |
| Texel fetch | 1042-1054 | indirect `Mipmap[mip][swizzledUV]` | scalar; **single biggest L1/L2 miss source** |
| Lightmap resolve (`resolvePixelLightmap`) | 1059 → L786 | reads `lightmapMF[i]`+`lightmapST[i]`, indexes scene `staticLMTable` | scalar; quick fall-through when off |
| Normal decode | 1069-1070 | `oct_decode_u16` | scalar |
| Normal-map TBN | 1090-1152 | only when `Mat->NormalMap` ≠ null; reads tangent G-buffer, TBN matmul, renormalize | scalar |
| Reconstruct view-space pos | 1157-1159 | `(0xFF80-zEnc)*invZScale` + projection | scalar |
| Ambient | 1163-1172 | Lumin + Diffuse×Ambient per channel | scalar |
| View dir (for spec) | 1196-1203 | `-pos / |pos|` via `fast_rsqrt` — only if `wantSpecular` | scalar |
| World-pos reconstruct | 1242-1250 | `viewToWorld * pos + cameraWorld` — needed for cube tap | scalar |
| Omni loop | 1252-1697 | one of: scalar branch-predicted (L1480+), vec 8-omnis (L1267+), or skip if `profNoLights`/`isWater` | scalar by default; vec only when `--deferred-vec` (off — slower on arm64) |
| **per-omni** Spot cone | 1499-1506 | scalar smoothstep | |
| **per-omni** 2D shadow tap | 1521-1640 | scalar 2×2 PCF on `sm.depth`+`sm.depth_dynamic`; OR polyId identity test on `sm.polyId`+`sm.polyId_dynamic` | scalar |
| **per-omni** Cube shadow tap | 1646-1657 | `resolveCubeAtten` → lightmap atlas (if applicable + `--shadow-lightmap`) else `CubeShadow_Sample` 4-tap PCF | scalar |
| **per-omni** Spec accumulate | 1666-1695 | half-vector + `pow_glossClass` | scalar |
| Saturate | 1701-1706 | 6 cmps | |
| Modulate | 1721-1726 | `tex*lit/256` | |
| Water composite | 1738-1746 | reads `out[i]` and half-adds | |
| Viz overrides | 1754-1772 | `--viz-normal` / `--viz-tangent` (dev) | |
| Final saturate + store | 1774-1781 | one `out[i] = ...` | |

So a typical greets pixel pays: 1 ZPage load, 1 mat32 load, 1 shadowMatID load, 1 normal
load, 0–2 nmap reads, 1 lightmap-G-buffer-pair load, 0–2 tangent loads, 1 ambient compute,
3 viewToWorld FMA chains, then for each omni in the tile list: ~6 FMAs (Lambertian) +
optional cone smoothstep + optional shadow tap (cube or 2D) + optional `pow_glossClass`.

### Shadow attenuation paths

| Light type | Path | Where | Cost |
|---|---|---|---|
| 2D spot shadow (Light_SpotLight + Omni_CastsShadow) | Scalar inline L1521-1640 (NOT in OuterVec vec body, NOT in TileFill, NOT in OuterVec spec fallback) | per-pixel-per-omni | 4 depth taps + bilinear weights + bias math |
| 2D spot polyId mode | L1579-1600 — replaces depth compare with id-not-equal-and-non-zero | per-pixel-per-omni | 4 polyId taps + identity tests |
| Cube tap (Light_Omni + Omni_CastsShadow + has cube ref) | `resolveCubeAtten` L813-951 | per-pixel-per-omni | as below |
| Cube + lightmap (Light_Omni + Omni_CastsShadow + Omni_StaticShadow + pixel on static mesh + `--shadow-lightmap`) | `pl.lm->sampleBilinear`/`sampleBilinearPlanar`/`sampleNearest` from `StaticShadowLightmap.h` (~L130-220) | per-pixel-per-omni | 4 atlas taps, bilinear blend, no cube projection |
| Cube + lightmap × dynamic (above + `--shadow-dynamic`) | composite: lightmap atlas × `CubeShadow_Sample(dynamicOnly=true)` | per-pixel-per-omni | 1 atlas tap + 1 dynamic cube tap |

**Critical**: the OuterVec kernel does NOT do 2D shadow taps in its vec body. The 2D shadow
attenuation only fires in the scalar `_Tile` and in the per-pixel branch reached via the
`deferred_vec=1` inner-vec path inside that scalar tile fn. The OuterVec's scalar fallback
(L2612-2694 for spec/water lanes) does the omni loop but **skips both shadow attenuation
calls entirely**. Cube shadows are present in `_Tile`'s standard scalar omni loop (L1646)
and the standard vec body (L1349-1378), but absent in OuterVec's omni loop and absent in
OuterVec's scalar fallback. This is consistent with `project_outervec_incorrect` note that
OuterVec matches std visually only on scenes without shadows.

## 3. Sub-pixel sampling modes

CLI flags: `--deferred-checkerboard` and `--deferred-quarter`. Env: `FDS_DEFERRED_CHECKERBOARD`,
`FDS_DEFERRED_QUARTER`. Both default off. Defined in `FeatureFlags.def` L22-23.

`deferredLightingQuarterEnabled()` wins when both are set (L767, L964).

### Checkerboard

Wave 1: `((px ^ py) & 1) == 0` → only even cells shaded. L1000 (scalar `_Tile`), L2261-2270
(OuterVec — uses lane mask).

Wave 2: `Render_DeferredLighting_TileFill` (L2728+) handles odd cells:
- For each odd pixel:
  - Left neighbor's matID and Right neighbor's matID compared against center matID (L2823-2825).
  - If both match: dword-level half-blend `((pL & 0xFEFEFEFE) >> 1) + ((pR & 0xFEFEFEFE) >> 1)`.
  - Else: full fallback shade (replays the wave-1 kernel for this pixel, L2841+).

Note: only the L/R neighbor pattern is implemented for checkerboard; no diagonal/vertical
forms.

User reports this looks acceptable.

### Quarter

Wave 1: `((px | py) & 1) == 0` → only `(even, even)` cells shaded. L1001 + L2272-2279.

Wave 2 patterns (L2754-2816), each requires all neighbors share matID with center; else
fallback:

| Cell parity | Pattern | Taps | Blend |
|---|---|---|---|
| odd_x, even_y | horizontal | `out[i-1]`, `out[i+1]` | `(pL>>1)+(pR>>1)` |
| even_x, odd_y | vertical | `out[i-XRes]`, `out[i+XRes]` | `(pT>>1)+(pB>>1)` |
| odd_x, odd_y | diagonal | 4 corners | `(pTL>>2)+(pTR>>2)+(pBL>>2)+(pBR>>2)` |

User reports: **"the robot looks cartoonish AND the floor looks blocky especially on far
pixels"**.

Likely contributing factors (descriptive, not prescriptive):

- The matID similarity test is on the per-pixel material from the **G-buffer**, not surface
  normals or depth. Two pixels can share matID but live on different faces with very
  different `nGeo` (e.g. opposite walls of a hex column). With 3 of 4 pixels reconstructed by
  averaging colors from same-matID neighbors that have very different lighting, depth-
  discontinuous and orientation-discontinuous boundaries get smeared. On the floor at far
  pixels the same matID covers a large area with steeply receding depth → blocky 2×2 patches
  of identical lit color.
- The fallback runs the **scalar wave-1 kernel** (L2841+), not the vec body, even when
  OuterVec is on. So a quarter wave-2 fallback pixel on greets pays the full scalar shading
  cost. The fallback also re-samples the normal map, ambient, etc — divergent code in a
  tight tile.
- The 4-corner diagonal blend has a 2-bit precision loss per channel.
- TileFill always runs scalar, both waves.

Memory tag `project_strict_fill_test_no_payoff` notes that adding normal/depth checks to
the fill pass cost ~2.5 ms with no visible quality gain; that route is parked.

## 4. Cube shadow tap — `CubeShadow_Sample` at `FILLERS/ShadowMap.h:210`

Steps per call:

| Step | Lines | Notes |
|---|---|---|
| Cube face select | L222 → `CubeShadow_SelectFace` L179-187 | dominant-axis on `worldP - lightWorldP`; 6 ops |
| ShadowMap fetch | L223 | one index `g_cubeShadowRefs[cubeIdx].faceIdx[face]` → `g_shadowMaps[..]` |
| `viewToLight` 3×3 matmul | L225-230 | 9 muls + 6 adds + 3 adds — produces `(lx,ly,lz)` in face view space |
| Near-plane reject | L236 | `lz <= 0.05f → return 1` |
| Face-frustum reject | L245-246 | `|lx|/lz, |ly|/lz` ≤ 1.5 |
| Projection | L247-249 | `smX = cntrX + perspX*lx/lz` and `smY` |
| NaN/sanity guard | L256-275 | `std::abort()` on insanely-out-of-range smX/smY; expensive only if it ever fires |
| Integer trunc | L276-277 | iX, iY |
| In-bounds reject | L278 | iX/iY against `sm.xres-1`, `sm.yres-1` |
| Bilinear weights | L280-285 | fx, fy, w00..w11 |
| **4 polyId taps from static buffer** (`sm.polyId`) | L306-307, 327-330 | 8 bytes (4×u16) |
| **4 polyId taps from dynamic buffer** (`sm.polyId_dynamic`) | L308-309, 336+ | 8 bytes |
| **4 depth taps from static** (`sm.depth`) | L310-311, 331-334 | 8 bytes |
| **4 depth taps from dynamic** (`sm.depth_dynamic`) | L312-313, 336+ | 8 bytes |
| `closestPoly` decision per tap | L316-324 (polyId), L364-367 (depth) | picks polyId of whichever buffer has the bigger occluder Z |
| Occlusion accumulate | L343-346 (polyId) or L368-371 (depth) | 4 cmps + 4 adds |
| Final blend | L373 | `return (occ >= 1) ? 0 : 1 - occ` |

User-noted measurement: ~32 ms total cube-tap time / ~1.44 M taps ≈ **22 ns / tap**, on
greets.

Cache footprint per tap (polyId mode): 4 × u16 polyId + 4 × u16 depth × 2 buffers = 32 bytes
of address space per buffer, but those 4 taps are at iX/iX+1 on rows iY/iY+1 — so 2 cache
lines per buffer × 4 buffers (2 polyId + 2 depth) = up to **8 cache lines per pixel per
omni**, with the four buffer streams (polyId static / polyId dynamic / depth static /
depth dynamic) for the same omni potentially hot at once. In depth mode the polyId loads
are skipped (only ~4 cache lines per pixel per omni).

Flags affecting it:

| Flag | Default | Effect |
|---|---|---|
| `shadow_polyid_no_pcf` | 0 | Single nearest-neighbor tap instead of 4 → ~9 ms saved on greets; jagged silhouettes |
| `shadow_polyid` | on | PolyId identity test instead of depth bias compare |
| `shadow_lightmap` | 0 | Skip cube tap on static mesh × static omni pixels; sample pre-baked atlas instead |
| `shadow_lightmap_planar` | 0 | Bake/sample atlas in world-axis-aligned planar projection |
| `shadow_lightmap_nearest` | 0 | Atlas: nearest instead of bilinear |
| `prof_no_cube_tap` | 0 | Short-circuit `resolveCubeAtten` → return 1.0 (full lit). The user's primary A/B handle. |
| `shadow_dynamic` | 0 | Each frame, re-bake animated meshes into `*_dynamic` buffers; lightmap path composites them on top via `dynamicOnly=true` cube tap |

## 5. Shadow render — `Render_DeferredShadowMaps` at `FDS/RENDER/Shadows.cpp:90`

The bake side. Called per frame from `GREETS.CPP:1850` (and 1856 for `shadow_dynamic`).

Three modes (`ShadowBakeMode`):

| Mode | Touches | Buffer |
|---|---|---|
| `StaticOnce` | static omnis (Omni_StaticShadow) | static `sm.depth`/`sm.polyId` |
| `DynamicOmnisPerFrame` | non-static omnis (mech-attached etc) | static `sm.depth`/`sm.polyId` (full rebake each frame) |
| `DynamicMeshesPerFrame` | static omnis, but **only animated meshes** projected; gated by per-mesh filter inside `Transform_Objects` flipped by `g_inDynamicShadowBake` | parallel `sm.depth_dynamic`/`sm.polyId_dynamic` |

Greets per-frame: `DynamicOmnisPerFrame` always (1850), `DynamicMeshesPerFrame` only when
`--shadow-dynamic` (1856).

`StaticOnce` runs once per scene init from `Initialize_Greets` → `ShadowMaps_BakeStatic` →
`FILLERS/ShadowMap.cpp:442`. Must happen **after** `Animate_Objects` so FLD-loaded omni IPos
splines resolve — see memory tag `project_static_bake_before_animate`.

Inside `Render_DeferredShadowMaps`:

| Phase | Lines | What | Threading |
|---|---|---|---|
| A: setup + Transform_Objects per light | 150-330 | Build per-light camera, swap `lightCtx`, clear depth+polyId buffers, enqueue `Transform_Objects` task per matching light | N tasks (1 per active shadow map) in threadpool |
| Barrier | 322-325 | Wait on `tileCounter` | |
| B: tile rasterize (light × tile) | 332-489 | Flat 6×4 tile job per light. Each task runs `clipper.Render(F, MekaleleShadowDepth, ...)` for each face that passed the mesh-level cull | N×24 tasks (flat) |
| Barrier | 490-494 | Wait | |
| Per-frame precompute `viewToLight` + `viewToLightOffset` | 549-565 | One affine per shadow map, used per pixel by the lighting kernel | single |

The flat (light × tile) batch in Phase B is intentional — straggler lights don't hold up
others. Mesh-level culling fires in `Transform_Objects` because:
- `g_currentShadowOmni` carries the omni pose so `sphereOutsideSpotCone` can reject meshes
  outside the cone.
- `g_currentShadowMap` carries `cubeFace` so the per-cube-face cull can pick the right face
  axis (gated on `--shadow-cube-face-cull`, default **on**).

The `Piramid` (greets wall mesh) chunking is set at scene init: one giant TriMesh becomes
N³ smaller TriMeshes via `--greets-piramid-chunk-grid=8` (default 8 → 512 cells, ~50-150
non-empty in practice). Without it the bsphere-vs-pyramid cull would never fire on the wall.

A `MatShadowCache` (L393-419) skips materials that look emissive (Mat_Transparent /
Mat_Additive / Mat_SkipZ or name-substring "lamp"/"emit*"). Lamp omnis don't self-occlude.

The shadow rasterizer is `MekaleleShadowDepth` (FILLERS/Mekalele.cpp / ShadowMap.h:93)
— depth-only (no color, no G-buffer, no texture). It's the rasterizer-side counterpart to
the deferred kernel's bilinear-PCF sampler.

`shadow_prof` (default off) prints per-frame Transform vs Raster cost rolling avg every
`FDS_SHADOW_PROF_INTERVAL` frames (60 default).

## 6. FeatureFlags inventory (perf-relevant subset)

All flags defined in `FDS/Base/FeatureFlags.def`. Forms: `--<name>`, `--no-<name>`,
`--<name>=value`, plus env `FDS_<NAME>`.

### Deferred

| Flag | Default | What | Note |
|---|---|---|---|
| `deferred` | FDS_DEFERRED_DEFAULT_ON | Master enable for deferred pipeline | |
| `deferred_zcull` | 1 | Cull G-buffer ROP writes against existing Z | |
| `deferred_vec` | 0 | 8-wide SIMD inner omni loop in scalar tile | Slower on arm64-via-simde |
| `deferred_outer_vec` | 0 (else Scene::PreferOuterVec) | 8 px × 1 omni outer kernel | Greets defaults off |
| `deferred_checkerboard` | 0 | Half-rate; see §3 | Works acceptably per user |
| `deferred_quarter` | 0 | Quarter-rate; see §3 | Robot cartoonish + floor blocky per user |
| `deferred_no_spec` | 0 | Skip specular | |
| `deferred_unified_tbr` | 0 | Unified transparent + particle TBR | |
| `deferred_gloss_stats` | 0 | Dump per-scene glossiness histogram | |
| `deferred_tile_stats` | 0 | Dump per-tile lighting cost stats | |
| `deferred_max_range` | 0.0 | Optional Range cap (cull only) | |

### Shadows

| Flag | Default | What |
|---|---|---|
| `shadows` | FDS_SHADOWS_DEFAULT_ON | Master |
| `shadow_polyid` | FDS_SHADOW_POLYID_DEFAULT_ON (on) | PolyId identity test vs depth compare |
| `shadow_polyid_no_pcf` | 0 | 1-tap nearest neighbor instead of 4-tap bilinear PCF |
| `shadow_backface_cull` | 0 | Cull back faces in shadow raster |
| `shadow_validate` | 0 | Capture clipper outputs outside input hull |
| `shadow_prof` | 0 | Per-light timing |
| `shadow_prof_cache` | 0 | Per-frame cache-line transition stats |
| `shadow_cone_cull` | 0 | Mesh bsphere vs spot cone cull |
| `shadow_skip_animated` | 0 | Static bake skips animated meshes |
| `shadow_dynamic` | 0 | Per-frame animated-mesh dynamic bake |
| `shadow_fzp_mult` | 3.0 | Shadow cam FZP multiplier over light IRange |
| `shadow_bias` | 512 | Constant Z bias |
| `shadow_slope_bias` | 1024 | Slope-scale Z bias |
| `shadow_lightmap` | 0 | Pre-baked per-face atlas; lighting reads atlas |
| `shadow_lightmap_res` | 16 | Atlas N×N per face |
| `shadow_lightmap_viz` | 0 | 1..9 different debug viz modes |
| `shadow_lightmap_recompute_bake` | 0 | Debug: skip atlas; call bake sampler per pixel |
| `shadow_lightmap_recompute_at_bary` | 0 | Debug: same but at runtime-stored bary |
| `shadow_lightmap_nearest` | 0 | Atlas nearest sample |
| `shadow_lightmap_planar` | 0 | World-axis-aligned planar projection bake/sample |
| `shadow_cube_face_cull` | 1 | Per-cube-face bsphere cull in Transform_Objects |
| `shadow_cube_vert_cull` | 0 | Per-vertex pyramid cull; only worth it for one giant moving mesh |

### Prof (diagnostic ablation gates)

| Flag | Default | What |
|---|---|---|
| `prof_no_tex` | 0 | Skip texture sample (uses 128/128/128) |
| `prof_no_lights` | 0 | Skip omni loop |
| `prof_no_spec` | 0 | Skip specular term |
| `prof_no_fog` | 0 | Skip fog pass |
| `prof_no_cube_tap` | 0 | `resolveCubeAtten` returns 1.0 |

These are the user's A/B handles; delta vs default = the gated stage's cost.

### Greets-specific

| Flag | Default | What |
|---|---|---|
| `greets_spot_height` | 0 | Override robot spotlight height (0 = scene default 4) |
| `no_greets_spots` | 0 | Skip code-installed robot + orbit spots |
| `greets_omni_shadows` | 0 | Mark all FLD omnis as `Omni_CastsShadow`+`Omni_StaticShadow` |
| `greets_omni_shadow_res` | 256 | Per-face cube map res for static FLD omnis |
| `greets_moving_omni_shadow_res` | 0 (=use above) | Per-face for moving omnis |
| `greets_piramid_chunk_grid` | 8 | N³ chunking of wall mesh; 8 → 512 cells |
| `greets_omni_default_range` | 1500 | Fallback IRange for FLD omnis with empty Range spline |

### Atmospherics (relevant because greets runs cones/halos by default)

| Flag | Default | What |
|---|---|---|
| `draw_cones` | 0 | Volumetric spotlight cones |
| `omni_halo_strength` | 0.0 | Halo brightness scale |
| `volumetric_unified` | 0 | Beer-Lambert unified pass |
| `vol_n_samples` | 4 | Ray-march samples |
| `vol_vec` | 1 | 8-wide SIMD per-sample inner loop |
| `vol_rect_cull` | 1 | Screen-rect cull per batch |
| `vol_halo_analytic` | 1 | Closed-form arctan integral |
| `vol_cone_analytic` | 1 | Cone analytic integral |
| `vol_prof` | 0 | Per-frame volumetric timing |
| `deferred_skybox` | 0 | Skybox-from-G-buffer pass |
| `fog_sigma_mult` | 3.0 | Beer-Lambert sigma |

### Display

| Flag | Default | What |
|---|---|---|
| `no_vsync` | 0 | Lets `SDL_RenderPresent` return immediately |

## 7. SIMD coverage map

| Operation | Path | SIMD | Width |
|---|---|---|---|
| G-buffer rasterize (Mekalele tile) | `FILLERS/Mekalele.h` | yes | 8-wide simde/AVX2-NEON; pixel-major SoA tiles |
| Mekalele octant normal encode | `oct_encode_u16_x8` (Mekalele.h:98) | yes | 8 |
| Mekalele octant normal decode | scalar in fallback, vec inline at L463-491 | partial | 8 in vec path; scalar in some leaf cases (`oct_encode_vec` memory tag) |
| Volumetric cones/halos per-sample loop | `Render_VolumetricCones_Tile` | yes (via `vol_vec`) | 8-wide pixel-major |
| Deferred fog pass | `Render_DeferredFogPass_Tile` L3328 | yes | 8 |
| Deferred lighting — scalar tile outer | `Render_DeferredLighting_Tile` | no | 1 |
| Deferred lighting — scalar tile inner omni loop | L1480+ | no | 1 |
| Deferred lighting — `deferred_vec` inner omni loop | L1267-1399 inside scalar tile | yes | 8 omnis × 1 pixel; off by default |
| Deferred lighting — OuterVec body | `_Tile_OuterVec` L2193+ | yes | 8 pixels × 1 omni |
| Deferred lighting — OuterVec normal-map TBN | L2400-2457 | no | per-lane scalar |
| Deferred lighting — OuterVec spec/water lanes | L2612-2694 | no | per-lane scalar fallback (no shadow taps) |
| Spec `pow(N·H, gloss)` | `pow_glossClass` (scalar tile) → template ladder of vec spec loops | yes for `{4,8,16,32,48,64,128}` | 8 omnis at a time |
| Cube shadow tap | scalar | no | 1 |
| 2D shadow tap (depth + polyId) | scalar | no | 1 |
| Atlas lightmap sample | `StaticShadowLightmap::sampleBilinear*` | no | 1 |
| Tile light list build | `buildTileLightLists` | partial | sphere-vs-tile cull is scalar |
| Shadow rasterizer | `MekaleleShadowDepth` (depth-only Mekalele) | yes | 8 — same pixel-major SoA |

## 8. Known issues, bugs, quirks

### Open

- **Shadow viz flicker even when paused** (task #83 created today). Mode is independent of
  scene tick, suggesting either the shadow-map dispatch is producing different output across
  frames despite identical scene state, or the overlay is reading from a buffer that's still
  being written by Phase B. `Render_DeferredShadowMaps` writes `g_shadowMaps[i].depth/polyId`
  with no read-side fence other than the Phase B barrier, but the V_Flip overlay
  (`ShadowMap_Overlay`) is also single-thread on the main thread after Render() returns.
  Still: needs investigation. Memory tag for follow-up if found: not yet created.
- **OuterVec kernel does not run 2D spot shadow attenuation** — the vec body in L2510-2566
  has no shadow tap, and the scalar fallback L2612-2694 also skips it. Cube shadows are
  similarly absent in both OuterVec paths. This is consistent with the description in
  memory tag `project_outervec_incorrect` (matches std visually only because greets uses
  cube shadows on static surfaces baked into the lightmap atlas, and most of the greets
  geometry actually goes through the scalar fallback anyway because of spec/nmap).
- **`std::abort()` in CubeShadow_Sample** (L274). If the smX/smY sanity guard ever fails the
  process dies. Per memory tag `feedback_no_defensive_backstops` this is intentional; called
  out here because it's not a typical fallback.

### Resolved / parked

- `project_outervec_incorrect` — OuterVec ~2 ms slower on greets (25 vs 23 ms); off by default.
- `project_strict_fill_test_no_payoff` — normal+depth checks on TileFill cost ~2.5 ms with
  no visible quality gain; reverted.
- `feedback_clipper_bary_lightmap` — Mekalele clipper invalidates per-pixel bary for atlas
  lookups when triangle is subdivided; `--shadow-lightmap-no-clipped` (default on) gates.
- `project_volumetric_simd_no_payoff` — pixel-major SIMD across rays wins ~21% at any N; on by default.
- `project_halo_analytic` — analytic atan integral replaces N-sample ray-march; on by default.

### Source-comment markers

`grep -rn 'TODO\|FIXME\|XXX'` in DeferredLighting.cpp + Shadows.cpp + ShadowMap.h returned
no hits — comments are mostly explanatory paragraphs, not deferred work markers.

### Quirks to keep in mind

- `Render_DeferredLighting` calls `getenv`-backed `FeatureFlags::xxx()` per pixel in
  multiple places. The accessors are cached-on-first-access, but per memory tag
  `feedback_use_feature_flags_for_hot_toggles` adding new hot-loop env reads must use
  FeatureFlags registry, not naked `getenv`. The kernel hoists every flag it touches to
  the top of `_Tile` (L963-994), but `resolveCubeAtten` re-reads several flags
  (`prof_no_cube_tap`, `shadow_lightmap_recompute_bake`, `shadow_lightmap_recompute_at_bary`,
  `shadow_lightmap_planar`, `shadow_lightmap_nearest`, `shadow_dynamic`) **per omni per
  pixel** when cube taps are reached. The FeatureFlags::xxx() calls are cached loads but
  not free, and the `prof_no_cube_tap` short-circuit is rechecked every tap. Same for the
  `g_shadowMode.load()` atomic in `CubeShadow_Sample` and `resolveCubeAtten`.
- TileFill always runs scalar even when wave 1 was OuterVec. Quarter-mode wave-2 cost is
  always paid in scalar.
- `Mat->Glossiness` not in `{4,8,16,32,48,64,128}` falls through to scalar libm `std::pow`
  in the `deferred_vec=1` path's default arm (L1452-1466). Should be impossible for shipping
  scenes (gated by `deferred_gloss_stats`).
- The greets per-frame call order is hard-coded in `GREETS.CPP::Tick_Greets`: Animate →
  greets-driver → Transform_Objects → Lighting → Radix_Sort → `gg->Render()` →
  `Render_DeferredShadowMaps(_, DynamicOmnisPerFrame)` → optional DynamicMeshesPerFrame.
  The shadow bake runs **after** the main Render() call, which means the lighting kernel
  inside Render() sees **last frame's** shadow maps. This is intentional (shadows trail by
  one frame to allow async on the next frame), but is worth flagging if assuming "this
  frame's bake feeds this frame's lighting".

  WAIT — re-reading `RENDER.CPP::renderFrame` and the Greets driver: `gg->Render()` is
  `fds::RenderPipeline::renderFrame` (RENDER.CPP:279). It contains `Render_DeferredLighting()`
  at line 371. So lighting fires *inside* Render(), then Greets calls
  `Render_DeferredShadowMaps` *after*. The first frame's lighting reads zero-initialized
  shadow maps; subsequent frames read N-1's bake. Confirmed by code; flagged as a perf
  surprise.

## 9. Measured numbers — today's snapshot

Greets `t=500`, 1920×1080, low-poly Piramid (5.5k faces), chunks on, cube-face cull on.

| Config | mean ms |
|---|--:|
| No shadows | 33 |
| Dynamic only (per-frame mech omni rebake; `--shadow-dynamic` off) | 90 |
| Lightmap only (`--shadow-lightmap` on; static omnis via atlas) | 73 |
| Both (lightmap + dynamic) | 89 |

Component decomposition:

| Component | ms |
|---|--:|
| Cube tap (delta from `--prof-no-cube-tap` at full shadows) | ~32 |
| Shadow render (`Render_DeferredShadowMaps` Phase A + B) | ~12.5 |
| Kernel "other" (everything in `_Tile` except cube tap + shadow render) | ~12 |
| Baseline-no-shadow (kernel + raster + fog + volumetric + flip etc) | 33 |

960×540 (quarter pixel count): ~33.6 ms.
- Frame floor (non-pixel-bound work + vsync alignment): ~16 ms.
- Pixel-bound work at 540p: ~17.6 ms.
- Linear extrapolation to 1080p: ~70 ms pixel-bound, matches the "both" measurement once
  shadow render is amortized.

Pixel-bound cost is therefore the dominant scaling factor. Resolution halving on each axis
divides the kernel + cube tap cost by ~4 directly.

Cube tap rate: 1920×1080 × ~5 omnis-per-pixel × ~0.15 hit-rate (after tile cull) → ~1.44 M
taps/frame at 32 ms → ~22 ns/tap. This includes the bilinear PCF + closestPoly + bounds
checks; the four cache-line read pattern at 64B/line × 4 lines × ~5 ns/L2-hit accounts for
most of it.

---

## 10. Flicker investigation

Symptom: with greets paused, the shadow viz overlay (B/V keys) shows small patches that
change frame to frame. Inputs (camera, omni positions, geometry, ShadowMatIDs) are constant
across paused frames, so the only way the overlay color can change is if the shadow buffer
(`sm.polyId` / `sm.polyId_dynamic`) itself changes — i.e., Render_DeferredShadowMaps
produces non-deterministic output for identical inputs. Candidates below are ordered by how
directly they explain "polyId differs frame-to-frame at fixed inputs."

### Candidate A — Shared tile-boundary pixels racing in `ShadowBarry::apply_exact` (HIGH)

**Where:** `FDS/FILLERS/ShadowMap.cpp:482-551` (`apply_exact`), dispatched from
`FDS/RENDER/Shadows.cpp:368-489` (Phase B tile loop).

**Why it's a candidate:** Phase B enqueues 24 tile lambdas per shadow map onto the shared
`ThreadPool`. The lambdas for one shadow map all read the same per-light `FaceListContext`
and all write to the same `sm.depth` + `sm.polyId` buffers. Each lambda's clipper limits its
output to its tile's `(x1f, y1f, x2f, y2f)` rect.

The shadow-pass tile rect is `tileSizeX = (sm.xres + 5)/6`. At sm.xres=512 that's 86 — not a
multiple of `TILE_SIZE=8`. So the clipper's per-tile rect boundaries fall *inside* an 8×8
ShadowBarry tile. Two adjacent tile-workers (e.g. tx=0 covering x∈[0,86) and tx=1 covering
x∈[86,172)) can both produce clipped polygons whose ShadowBarry tile loop touches the
global 8×8 tile at x∈[80,88) — worker 0 wants to write pixels 80-85, worker 1 wants 86-87.

`apply_exact` at line 530 does `*(__m128i*)zRow = _mm_blendv_epi8(*(__m128i*)zRow, encU16,
maskU16)` — a 16-byte read-modify-write of the whole 8-uint16 row chunk. This is *not*
atomic. If worker 0 reads the 16-byte word, worker 1 reads it concurrently, both blend
their own lanes, and both write back, one worker's lanes are silently lost. Same chunk also
covers the polyId scalar loop at lines 535-540, but those are independent 1-uint16 stores
per lane, so polyId per-pixel race is only an issue when *the same* pixel is masked-in by
*both* workers (i.e., a vertex landed exactly on the shared edge after clipper rounding,
which `lroundf(PX * 16)` will produce often when both triangles' edge passes through the
same integer subpixel coord).

When two coplanar triangles with *different* `ShadowMatID` (greets wall split assigns one
per cluster) share an edge that the clipper cuts across the boundary, the shared-edge
texel's polyId is whichever worker wrote last. With pool scheduling non-deterministic, the
winner flips frame-to-frame even at fixed inputs → patches at cluster seams.

This also explains why city hasn't shown the same flicker: city has fewer `ShadowMatID`
distinctions (no wall-split), so the polyId of the two contenders is usually the same and
the race is invisible.

**How to verify:**
1. Pause greets, B-key the overlay on, then run with `--shadow-tiles=1` (or any flag that
   forces serial Phase B tile execution) and see if flicker disappears. If yes → race.
2. Or: after `Render_DeferredShadowMaps` returns, FNV-1a hash `sm.polyId` for one map and
   log it. If two consecutive paused frames produce different hashes, polyId is being
   written non-deterministically — already confirmed by the `[SHADOW-STALE]` machinery in
   `ShadowMap.cpp:60-98`, just run it under `T` key while paused.
3. Sanity check: change `tileSizeX/Y` to be rounded UP to a multiple of `TILE_SIZE` (8) so
   the per-tile clip rects align with ShadowBarry's 8×8 grid. Flicker should disappear.

**Likelihood:** HIGH. Symptom (patches flicker on pause) matches exactly: race winner
varies with pool scheduling, identical inputs aren't enough to fix the output.

### Candidate B — `tile.x * TILE_SIZE` offset is global, but tiles aren't pixel-aligned (HIGH, same root as A)

**Where:** `FDS/FILLERS/ShadowMap.cpp:483-489`.

```cpp
uint16_t * const zRowBase  = zArr
    + size_t(tile.y) * barry::TILE_SIZE * size_t(xres)
    + size_t(tile.x) * barry::TILE_SIZE;
```

`tile.x` is in 8-pixel units of the *global* shadow map. So workers from different clipper
rects writing to the same global `tile.x` are aliasing the exact same memory region. This
is the second face of (A) — the writes don't just go to nearby memory, they go to *identical*
16-byte words.

Combined with the non-8-aligned clipper rects, two workers WILL touch the same word at
seams.

**How to verify:** Print `(tile.x, tile.y, worker_thread_id)` for every `apply_exact` call
for one shadow map's frame and check for duplicates. Any (tile.x, tile.y) pair from two
different workers proves the aliasing.

**Likelihood:** HIGH. Same root cause as A.

### Candidate C — `MekaleleShadowDepth` skipMipLevel + clipper bary edge case (MEDIUM)

**Where:** `FDS/FILLERS/ShadowMap.cpp:866-907` (clipper validation block), `Shadows.cpp:466`
calls `clipper.Render(..., skipMipLevel=true)`.

**Why it's a candidate:** The `--shadow-validate` block logs clipper outputs whose verts
escape the input triangle's convex hull. There's a memory note about [Mekalele clipper
invalidates per-pixel bary for lightmap lookups](feedback_clipper_bary_lightmap.md) — if a
similar bary-mismatch path is firing for the shadow rasterizer (per-pixel polyId stamp
written based on a face whose bary doesn't actually cover the texel), the polyId at edge
texels could be the *wrong* face's ID, which then races with the correct face's per-tile
write. That makes the patches more visible (correct vs wrong cluster) instead of just
"different cluster on each tie."

But the bary itself is constant across frames for fixed geometry — so this alone wouldn't
explain "different frame to frame." It only amplifies the racing in A/B.

**How to verify:** Run with `--shadow-validate`. If `[SHADOW-CLIP]` lines spam during
paused playback for the same tile each frame, the clipper is producing extra verts; if they
fire only intermittently, it's interacting with the race.

**Likelihood:** MEDIUM. Plausible amplifier of A/B but not the root cause of the time-
varying behavior.

### Candidate D — Stale `g_cubeShadowRefs[*].lightISource` mismatch with `sm.lightISource` (MEDIUM/LOW)

**Where:** `FDS/RENDER/Shadows.cpp:101-103` updates `cr.lightISource = cr.omni->IPos` at the
top of every shadow-render call. `sm.lightISource = O->IPos` is updated again per-light in
Phase A loop at line 249. These are two separate copies and both must agree for
`CubeShadow_Sample` to return correct values (lines 219-223 of `ShadowMap.h`).

The update at line 101-103 iterates `g_cubeShadowRefs` AT ENTRY, before checking the mode
or the omni's active flag. So even if Render_DeferredShadowMaps does nothing for this omni
this frame (wrong mode), cr.lightISource still gets refreshed from the current IPos. Phase
A updates `sm.lightISource` only for omnis included in the mode filter. If the omni is
active for one mode call (DynamicOmnis) but not the other (DynamicMeshes), the two
buffers are in sync after the first call but Phase A's lightISource update from the second
call doesn't run for this omni → still in sync. So this isn't a bug source.

**Why low:** This wouldn't be time-varying under pause (IPos is constant when paused).

**How to verify:** Skip — not a likely flicker source.

**Likelihood:** LOW.

### Candidate E — `thread_local` capture-by-pointer of main thread's `perLightFaces` etc. (LOW)

**Where:** `FDS/RENDER/Shadows.cpp:139-142` declares `static thread_local
std::vector<fds::FaceListContext> perLightFaces;` and friends. Lambdas at lines 300-319
and 376-486 capture pointers (`facesPtr`, `scratchPtr`, `camPtr`) into these vectors.

**Why it's a candidate:** thread_locals live in the thread that called the function. The
main render thread calls `Render_DeferredShadowMaps`, so `perLightFaces` lives on the main
thread. Worker threads dereference pointers that point into another thread's TLS.

The C++ memory model says this is OK as long as the source thread doesn't destroy the
storage before workers finish reading. The barrier between Phase A and Phase B + the final
Phase B barrier guarantee this. Lifetimes are also fine — `static thread_local` lasts the
whole program. So no UAF.

But: if a second concurrent caller existed (e.g. a debug path that called
Render_DeferredShadowMaps from another thread), they would have independent TLS copies and
weird things would happen. As of the current code path, only the main thread calls it, so
no race here.

**How to verify:** Skip — not a flicker source as written.

**Likelihood:** LOW (latent risk only).

### Candidate F — `ShadowMap_TickStalenessTracker` reads `polyId_dynamic` as `uint8_t` (LOW, diagnostic only)

**Where:** `FDS/FILLERS/ShadowMap.cpp:75`: `for (uint8_t v : sm.polyId_dynamic)`. The vector
is `vector<uint16_t>` (header line 49). Iterating with `uint8_t v` truncates each entry.

**Why low:** This is a hash computed for the diagnostic `[SHADOW-STALE]` print only. It
doesn't write to `polyId_dynamic` and doesn't affect rendering. But it does silently lose
the upper byte of each polyId, so the stale-tracker will sometimes report "unchanged" when
only the high byte changed → understates actual non-determinism. Worth fixing alongside
because it'd mask future bug repros.

**Likelihood:** LOW for visible flicker, BUT it would hide regressions in the staleness
tracker.

### Candidate G — `sm.fzp` / `sm.zScale` set at scene init but `fzp` used by Phase A clear / Render (LOW)

**Where:** `ShadowMap.cpp:371,419` set `sm.fzp = O->IRange * sFzpMult` and `sm.zScale =
0xFF00 / (sm.fzp * 1.1f)` at Rebuild. `Shadows.cpp:254` overwrites `sm.zScale` per frame.

**Why low:** Per-frame overwrite is deterministic given constant inputs. Not a flicker
source unless `O->IRange` itself drifts when paused (it doesn't — IRange isn't on the
animation spline).

**Likelihood:** LOW.

### Most likely root causes

1. **Candidate A/B (same bug, two angles)**: ShadowBarry's per-tile `_mm_blendv_epi8`
   read-modify-write on global 8×8-pixel words combined with non-8-aligned Phase B tile
   rects (`tileSizeX = (sm.xres + 5)/6 = 86 @ 512²`) lets two tile-workers concurrently
   stamp the same texel with different ShadowMatIDs. Race winner varies with pool
   scheduling → patches flicker frame-to-frame even when inputs are frozen.
2. **Candidate C** (clipper bary edge case) is a plausible amplifier — wrong-face polyId at
   the seam makes the visible delta larger when the race winner flips — but it's a
   secondary, not the cause of the time-varying behavior.

---

## Quick index

- Pipeline orchestration: `FDS/RENDER/RENDER.CPP::renderFrame` (L279), `GREETS.CPP::Tick_Greets` (L~1700)
- Deferred entry: `FDS/RENDER/DeferredLighting.cpp:3032`
- Scalar kernel: `DeferredLighting.cpp:953`
- OuterVec kernel: `DeferredLighting.cpp:2193`
- Wave-2 fill: `DeferredLighting.cpp:2728`
- Cube tap: `FDS/FILLERS/ShadowMap.h:210`
- Cube tap dispatch (lightmap vs cube): `DeferredLighting.cpp:813`
- Shadow render: `FDS/RENDER/Shadows.cpp:90`
- Per-pixel 2D shadow tap (scalar): `DeferredLighting.cpp:1521-1640`
- Dispatch logic OuterVec: `DeferredLighting.cpp:737`
- Sub-pixel dispatch: `DeferredLighting.cpp:752,767`
- Fog pass: `DeferredLighting.cpp:5411`
- FeatureFlags: `FDS/Base/FeatureFlags.def`
- Lightmap atlas sampler: `FDS/Base/StaticShadowLightmap.h:130-220`
