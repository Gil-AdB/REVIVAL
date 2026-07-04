# Graphics pipeline — how it's wired

A map of the **deferred rendering path** for anyone adding or debugging a graphics
feature (a post-pass, an effect, a G-buffer consumer). It captures the wiring you'd
otherwise have to re-derive each time: the per-frame call order, the G-buffer layout,
the view-space reconstruction math, the HDR buffer + tonemap, the FeatureFlags system,
the canonical tiled post-pass recipe, headless validation, and the gotchas that cost
time. For the broader engine (control flow, rasterizers, data model, threading, wasm)
see [ENGINE.md](ENGINE.md); for live perf numbers see [PERF_STATE.md](PERF_STATE.md).

> Line numbers drift — they're anchors, not contracts. Grep the symbol if it moved.

---

## 1. Two rendering paths: forward vs deferred

The engine has a **forward** path (per-vertex `Lighting()` + rasterizer colorize) and
a **deferred** path (G-buffer fill → per-pixel lighting kernel). The demo runs deferred
when `--deferred` is set; greets/city/etc. assume it. The deferred path is where all
modern effects live (fog, volumetrics, HDR, mirrors, and now SSAO).

Everything below is the **deferred** path. In forward mode the G-buffer isn't sized,
so any G-buffer consumer must early-out (`if (!g_gbuffer || g_gbuffer->normal.size() < N) return;`).

---

## 2. Per-frame call order (`renderFrame`)

`FDS/RENDER/RENDER.CPP` → `fds::RenderPipeline::renderFrame()` (~`RENDER.CPP:338`). The
deferred branch, in order:

| Step | Call (≈line) | What it does |
|---|---|---|
| HDR begin | `Hdr_BeginFrame()` `:470` | size+clear the f32 radiance buffer `g_hdrBuf` (only if `--hdr`) |
| Shadow join | `ShadowBake_JoinPending()` `:476` | join async dynamic-shadow bake |
| **Lighting kernel** | `Render_DeferredLighting(dctx)` `:485` | per-pixel ambient+diffuse+spec+shadows. Writes VPage (LDR) **and** `g_hdrBuf` (if `--hdr`) |
| **SSAO** | `Render_SSAO()` `:494` | example post-pass: modulates lit radiance (see §6) |
| Deferred sky | `Render_DeferredSkybox()` | paint untouched (sky) pixels |
| **Fog / froxels** | `Render_DeferredFastFog(dctx)` `:523` | analytic + froxel volumetric fog; **activates HDR** when `--fast_fog` |
| HDR activate (fog-off) | `Hdr_ActivateNoFog()` `:539` | greets has no fog → activate HDR here so the tonemap fires |
| Transparent peel | per-mesh, ~`:560+` | 2-layer xpar G-buffer, lit onto VPage / `g_hdrBuf` |
| Volumetric cones/halos | `Render_VolumetricCones` / unified | god-rays; accumulate into `g_hdrBuf` |
| Sprites / particles / TBR | `TBR_Render` | back-to-front transparent flush |
| **Tonemap** | `Render_TonemapToVPage()` `:816` | `g_hdrBuf` → VPage (ACES). The LAST HDR write wins (see §4) |
| Post (LDR) | lens / grade / grain | chromatic aberration, vignette, film grain on final VPage |

**Where to insert a new pass:** if it darkens/modulates **surfaces**, put it right after
the lighting kernel (like SSAO). If it's **additive glow** that should bloom, put it
before the tonemap so ACES rolls it off. If it's a **2D screen effect on the final
image**, put it after the tonemap.

**`skipVolumetric`** is true for the city water-reflection / mirror-RTT underlay passes.
Those render into a *different* target and must NOT touch `g_hdrBuf` (it's sized for the
main view). Gate main-only passes with `if (!skipVolumetric)`.

---

## 3. The G-buffer (`meka::GBuffer`)

`FDS/FILLERS/Mekalele.h:39`. Per-pixel, `XRes*YRes`, contiguous (no scanline pad).
Global handle: `extern meka::GBuffer *g_gbuffer;` (`Mekalele.h:980`); transparent layers
are `g_gbufferTransparent` / `g_gbufferTransparentBack`.

| Field | Type | Meaning |
|---|---|---|
| `normal` | `u16` | **octahedral-packed view-space shading normal**. Decode with `meka::oct_decode_u16` (`Mekalele.h:199`), encode with `oct_encode_u16` (`:110`) |
| `tangent` | `u16` | oct-packed view-space tangent (normal-mapped materials only) |
| `txtr` | `u32` | `miplevel:4 | matID:8 | swizzled UV:20`. matID doubles as per-pixel surface ID |
| `lightmapMF` / `lightmapST` | `u32`/`u16` | static-shadow lightmap address + barycentric |
| `shadowMatID` | `u16` | per-pixel shadow receiver identity |
| `mirrorId` | `u8` | per-pixel planar-mirror ownership |

**Depth is NOT in the GBuffer struct** — it's the global `ZPage16` (`word*`, 16-bit). The
kernel reconstructs view-space position from `ZPage16` + screen XY (§5). `ZPage16[i] == 0`
means "pixel not touched by the rasterizer" = sky/background.

---

## 4. HDR buffer + tonemap

`FDS/RENDER/Hdr.{h,cpp}`. `extern std::vector<float> g_hdrBuf;` (`Hdr.cpp:23`) —
**f32, channel order B,G,R,(pad)**, 4 floats/pixel, contiguous (`row = y*W*4`).

Flow when `--hdr` is on:
1. `Hdr_BeginFrame()` sizes + clears it and records `g_hdrBufW/H`.
2. The lighting kernel writes **unclamped linear radiance** into `g_hdrBuf` for covered
   opaque pixels (plus a coverage flag in the pad channel `h[3]`).
3. Fog / cones / transparents / bloom accumulate more radiance.
4. `Render_TonemapToVPage()` (`Hdr.cpp:636`) applies exposure → ACES → (linear path:
   sqrt encode) and writes the 8-bit VPage. **It no-ops unless `g_hdrActive`.**

`g_hdrActive` is set by the fog composite (`--fast_fog`) or by `Hdr_ActivateNoFog()`
(fog-off scenes like greets). Until then the buffer holds data but the tonemap won't fire.

**The #1 HDR gotcha:** a pass that writes **VPage** *before* the tonemap is **silently
discarded** under `--hdr`, because the tonemap overwrites VPage from `g_hdrBuf` at the
end. greets/`--cinematic` default to HDR. So a surface-modulating pass must write
`g_hdrBuf` when HDR is active and VPage otherwise:

```cpp
const bool useHdr = fds::FeatureFlags::hdr() && fds::Hdr_WritableFor(W, H);
// useHdr → multiply g_hdrBuf[i*4 + 0..2] (B,G,R linear); leave h[3] (coverage) alone.
// else   → modulate VPage (8-bit BGRA) in place.
```

Gate on `Hdr_WritableFor(W,H)` (buffer sized for *this* view), **not** `g_hdrActive` —
right after the lighting kernel the radiance is present but activation hasn't run yet.
`Render_SSAO` in `DeferredSSAO.cpp` is the worked example.

---

## 5. View-space reconstruction (the canonical math)

Every G-buffer consumer reconstructs view-space position the same way the kernel does
(`DeferredSurfaceKernel.cpp:651`):

```cpp
const float z = float(0xFF80 - zEnc) * invZScale;          // invZScale = 1/g_zscale
const float x = (float(px) - CntrEX) * z * invFOVX;        // invFOVX = 1/FOVX
const float y = (CntrEY - float(py)) * z * invFOVY;        // note the Y flip
```

Inverse (re-project a view-space point back to a pixel):

```cpp
const int spx = (int)(CntrEX + (X / Z) * FOVX + 0.5f);
const int spy = (int)(CntrEY - (Y / Z) * FOVY + 0.5f);
```

Globals live in `FDS/Base/FDS_VARS.H`: `FOVX/FOVY`, `CntrEX/CntrEY`, `XRes/YRes`,
`ZPage16`, `VPage`, `g_zscale`. View-space normal: `meka::oct_decode_u16(g_gbuffer->normal[i], nx,ny,nz)`.

**Scene-scale gotcha (cost a tuning round on SSAO):** these scenes live in **view-Z ≈
[5 .. 80] units**, not thousands. Any world-space radius/distance constant must be sized
to that. A "reasonable-sounding" radius of 24 is a third of the entire depth range and
overshoots all geometry. When a depth-based effect looks inert, **dump the live view-Z
range** before guessing (SSAO has `FDS_SSAO_STATS=1` for exactly this).

---

## 6. The canonical tiled post-pass

Every full-screen deferred post-pass uses the same 6×4 tile-job pattern over the shared
threadpool. Template (from `DeferredFastFog.cpp` and `DeferredSSAO.cpp`):

```cpp
namespace renderns { extern std::counting_semaphore<INT_MAX> tileDone; } // DeferredFastFog.cpp:50

constexpr int numTilesX = 6, numTilesY = 4;
const int tsx = (XRes + numTilesX - 1) / numTilesX;
const int tsy = (YRes + numTilesY - 1) / numTilesY;
for (int tj = 0; tj < numTilesY; ++tj) {
    const int y1 = tsy*tj, y2 = std::min(y1+tsy, (int)YRes);
    for (int ti = 0; ti < numTilesX; ++ti) {
        const int x1 = tsx*ti, x2 = std::min(x1+tsx, (int)XRes);
        ThreadPool::instance().enqueue([=]() {        // capture BY VALUE — no dangling refs
            for (int py = y1; py < y2; ++py)
              for (int px = x1; px < x2; ++px) { /* ... per-pixel ... */ }
            renderns::tileDone.release();
        });
    }
}
for (int n = numTilesX*numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
```

Notes:
- Capture `[=]` (by value); the enqueuing thread's stack frame is gone by the time tiles run.
- `renderFrame`/scene ticks run on a non-pool thread, so enqueue+wait can't deadlock.
- Reusing `renderns::tileDone` is fine for passes that never overlap. Don't share it
  across concurrent waves.
- Per-pixel hot toggles must use cached `FeatureFlags::x()` reads, **never `getenv` in the
  loop** (it tanked greets to seconds/frame once).

---

## 7. FeatureFlags (every runtime knob)

`FDS/Base/FeatureFlags.def` is an X-macro table; `.h`/`.cpp` generate the enums + accessors.
One line registers a flag **and** its CLI `--name` / env `FDS_NAME` / help text:

```
FDS_FLAG_BOOL (name, "FDS_ENV", default, "category", "help…")
FDS_FLAG_FLOAT(name, "FDS_ENV", default, "category", "help…")
FDS_FLAG_INT  (name, "FDS_ENV", default, "category", "help…")
```

Read it anywhere as `fds::FeatureFlags::name()`. CLI `--name=val` / `--name` / `--no-name`
and env `FDS_ENV=val` both work; CLI/env always win over scene defaults.

**Per-scene defaults** use `FeatureFlags::setDefault(Id, val)` — it only writes if the
user *didn't* set the flag (`GREETS.CPP` ~`:974`). **Global render flags must go in the
scene's run-defaults hook** (e.g. `GreetsApplyRunDefaults`), NOT in `Initialize_*`, or
they leak across the demo sequence.

Gotcha: a flag literally named `no_foo` collides with `--no-foo` negation — smoke-test any
new `no_*` flag from the CLI.

---

## 8. Headless validation (no SDL window)

The harness can A/B any deferred change deterministically without a window. Prefix with
`SDL_VIDEODRIVER=dummy` so no window pops and output is byte-identical to a real run.
**Run from `Runtime/`** (asset paths are CWD-relative). The CMake POST_BUILD already
copies the binary to `Runtime/DEMO`, so just rebuild — no "pull".

```sh
cd Runtime
SDL_VIDEODRIVER=dummy ./DEMO --deferred --snapshot=greets@t=500 --out=/tmp/a            # baseline
SDL_VIDEODRIVER=dummy ./DEMO --deferred --ssao --snapshot=greets@t=500 --out=/tmp/b     # change
```

Snapshot scenes: `greets`, `city`, `chase`, `fountain`, `crash`, `conetest`, `mirrortest`
(see `DEMO/Snapshot.cpp` + `REV.CPP` `--help`). Each writes `<scene>_t<NNNNNN>_color.ppm`
(+ a `_z.pgm`). Compare with a byte diff / PIL; **don't eyeball thumbnails** for subtle
changes — crop the region at full res and diff numerically. `tools/render_gate.sh` is the
multi-pose gate for "byte-identical" refactors.

**HDR note for validation:** greets defaults to HDR, so a VPage-only effect will show
**zero diff** there — that's the §4 gotcha, not a broken effect. Verify with `--no-hdr`
or by confirming the effect writes `g_hdrBuf`.

---

## 9. Worked example — SSAO (`FDS/RENDER/DeferredSSAO.cpp`)

A complete, recent post-pass that exercises every section above:
- Reads `g_gbuffer->normal` (oct-decoded view normal) + `ZPage16` (depth).
- Reconstructs view position with the §5 math; samples a hemisphere kernel, re-projects
  each sample to a pixel, compares stored depth → occlusion.
- Output target auto-selects per §4: multiplies **linear `g_hdrBuf`** under `--hdr`
  (physically correct — AO scales radiance before ACES, and glow added later isn't
  occluded), else multiplies VPage.
- Reduced-res ladder: `--ssao_downscale 1|2|4` computes AO on a `W/d × H/d` grid and
  depth-aware-bilinear-upsamples; the depth buffer is always full-res so edges stay crisp.
- **Noise/denoise (the floor-banding saga — read before touching it):** the kernel
  rotation is a **4×4 tiling** pattern (16 golden-ratio angles, cell `x,y mod 4`) paired
  with a **matched 4×4 box denoise** (offsets −2..+1 = exactly one period). The blur then
  averages each of the 16 rotations once and cancels the rotation variance on flat
  surfaces. A *continuous* per-pixel rotation (the original interleaved-gradient angle)
  never repeats, so no finite blur resolves it → a faint diagonal **"hatch"** on the
  grazing floor, sample-count-independent (still there at 256 spp). Things that were
  **tried and REVERTED** (don't re-attempt):
  - `cos⁴(shading-normal)` weight in the denoise/upsample → **bands on normal-mapped
    surfaces**: the G-buffer normal is bump-perturbed, so neighbours disagree and the
    filter down-weights its own taps. A correct crease-preserving weight needs a *smooth
    geometric* normal (pre-normal-map), which the G-buffer doesn't store.
  - geometric-normal **plane-distance** weight (iq's SSAO denoise) → **0 hatch gain here,
    ~2× denoise cost**. Measured, dropped.
  - normal-aware **upsample** → same normal-map banding + ~+1.8 ms; edge-gating it deopts
    the apply loop's auto-vectorization (~1.2 ms regardless). Reverted.
  The residual ~0.9 hatch (debug AO, 6–10× stretched) is inherent low-amplitude AO noise —
  invisible at normal contrast once composited. `--ssao_samples` (default 16) does NOT fix
  it (it's an inter-pixel pattern, not per-pixel variance); only the matched blur does.
- Flags `--ssao*` are auto-derived from `FeatureFlags.def`; `FDS_SSAO_STATS=1` prints the
  per-pass timing, live view-Z range + AO histogram.
- **Scene scale gotcha:** view-Z ≈ [5..80] units, so `--ssao_radius` ~3–5 (NOT 24, which
  overshoots all geometry → *less* occlusion). See §5.
- **Two compute producers (Pass 1):** the default **hemisphere point-sampler** (8-wide SIMD)
  and **GTAO + Visibility Bitmask** (`--ssao_gtao`, scalar, opt-in). They write the same
  `aoRaw`/`aoZ`, so the denoise + apply downstream are shared — GTAO is a true drop-in.
  - The hemisphere sampler has the classic **"halo"**: a foreground object over-occludes a
    fat band of background (the sphere reaches past the silhouette), darkening the floor far
    past real contact AND leaving the silhouette edge under-occluded → a **bright rim
    relative to the darkened surroundings**. Note SSAO only ever *darkens* per-pixel — the
    bright halo is a *contrast* effect (darkened floor next to an un-darkened edge), so a
    diff showing "edge unchanged, surroundings darker" IS the halo, not its absence.
  - **GTAO fixes it**: horizon integration occludes only by the occluder's true angular
    extent → tight contact-defined AO, no fat band, no bright rim. The 32-sector bitmask +
    `--ssao_gtao_thickness` lets light pass behind thin occluders (the legs). Knobs:
    `--ssao_gtao_slices` (dirs), `--ssao_gtao_steps` (march), `--ssao_gtao_thickness`.
  - GTAO compute is **8-wide SIMD** (vectorized per-sample arithmetic + bitmask via
    `_mm256_sllv/srlv`; gather + popcount stay scalar-per-lane). ~2.7× over scalar →
    **on par with the hemisphere sampler** (d4 ~4ms, d2 ~10ms total). `FDS_SSAO_NOSIMD`
    forces the scalar reference for A/B. SIMD matches scalar within rsqrt/cvt rounding
    (max ~6/255, denoised away).
  - **A bright edge halo is NOT always SSAO** — full-screen **bloom/anamorphic** blur the
    blown-out floor over a dark silhouette too (non-depth-aware), an *additive* rim that
    persists with `--no-ssao`. Diagnose by toggling `--no-ssao` (SSAO halo = contrast,
    vanishes) vs `--no-bloom --no-anamorphic` (post halo = additive, vanishes).

### SSAO performance — what was done, and what didn't pay

Three passes (compute → denoise → apply), tiled 6×4 over the threadpool. Cost levers,
in the order they were applied (greets 1080p quarter-res, `FDS_SSAO_STATS=1`):

| change | effect |
|---|---|
| `--ssao_samples 8` default | compute ∝ samples; 8 ≈ 16 visually (denoise hides grain) |
| divide-free bilateral weights (denoise+apply) | `1/(1+x²)` per tap → `max(0,1−x²·k)`; killed the full-res denoise cost |
| 8-wide SIMD compute (`_mm256_rcp_ps`, `fast_rsqrt`) | ~20–25% on compute (gather-bound caps it — the per-sample depth lookup stays scalar; cf. the volumetric-SIMD note) |
| Newton-Raphson on the projection rcp | **dropped** — measured byte-≈identical (≤3/255) and free-but-pointless |

Net: **5.8 → 3.7 ms** at quarter-res, all correct. The **apply (~1.7 ms) is now the floor**,
~0.7 ms of which is the unavoidable `g_hdrBuf` read-modify-write bandwidth.

**Approx reciprocals:** the SSAO compute uses raw `_mm256_rcp_ps` (12-bit) for the
per-sample projection and range divides — at 1080p that's ~0.25 px of tap drift, which
rounds to the same depth pixel. Audit before trusting rcp in *high-gain* contexts (the
narrow-cone moire family, see `DeferredCommon.h::rsqrt_nr_x8`); SSAO is low-gain so it's safe.
NB `oct_decode_u16` has **no** runtime divide (`1/127` is const-folded, normalize is
`fast_rsqrt`). `oct_encode_u16_x8`'s L1-normalize `_mm256_div_ps` was switched to raw
`_mm256_rcp_ps` (commit `026a211`) — byte-near-identical (max 2/255 on ≤0.016% of pixels,
nothing visible), with the dead-code scalar `oct_encode_u16` reference moved to the same
`_mm_rcp_ss` estimate (proven bit-identical) so both paths agree. **Frame-level perf was
in the noise on arm64** — the encode is already vectorized (one reciprocal per 8 px), so
the stale "#1 hot rasterizer line" comment (pre-vectorization) overstated it. Kept anyway
deliberately: x86 has a much wider `divps`/`rcpps` gap than NEON, so it may be a real gain
on an Intel/AMD or wasm→x86 build (don't revert it reading only "neutral"). Lesson: this hunt also
showed incremental `ninja` + edit churn can give phantom diffs — `touch` the header to
force a clean rebuild before trusting a byte-comparison.

### Fusion experiment (kernel-fold) — measured, reverted, does NOT pay

The apply's ~0.7 ms is a separate full-res sweep of `g_hdrBuf`. The TBR/locality instinct:
fold the AO multiply into a pass already touching those pixels. The **lighting kernel** is
the only *correct* target (the tonemap runs after the volumetric glow composites into
`g_hdrBuf`, so fusing there would wrongly darken the disco beams). A prototype (SSAO
compute+denoise before lighting, kernel multiplies AO into its `g_hdrBuf` write) was built,
measured, and **reverted** — it's not in the tree, this is the recorded finding.

**Result: it loses.** Adding a per-pixel AO sample to the hot *scalar* kernel cost more
than the apply sweep it removed — full-rate kernel **+5.4 ms** vs ~1.6 ms apply saved
(**net +3.8 ms**); quarter-rate ~break-even-to-slightly-worse. This is the documented
"merging into the tuned kernel regresses" trap. **Keep the standalone apply.**

The win is real only at the *architectural* level: a **tile-resident deferred pass** that
fuses lighting → SSAO → fog → cones → tonemap so `g_hdrBuf` is produced+consumed in cache
and barely hits DRAM (the `RENDER_DAG_SCOPING.md` direction). SSAO is one stage of that,
not a standalone graft.

### SSAO + PBR AO maps (future)

A baked **AO map** is the natural companion to SSAO, and the two are *complementary, not
redundant*:

- **AO map** — baked, static, *material-scale* occlusion from the asset's high-poly bake
  (pores, panel-seam recesses, bolt holes). Detail SSAO physically **cannot** see: it's
  sub-pixel, or occluded by geometry the G-buffer doesn't hold.
- **SSAO** — dynamic, view/scene-dependent, *inter-object* and large-scale occlusion (one
  object's contact shadow on another). No baked map can capture this.

PBR combines them: `ambient *= aoMap × ssao` (or `min`), applied to **ambient/indirect
only** — direct light keeps its own shadows. (Today's standalone SSAO multiplies *total*
radiance, which is cheaper but slightly wrong; moving to ambient-only is the PBR-correct
shift and changes the look a touch — less aggressive.)

What AO maps would buy here:
1. **Fill the detail reduced-res SSAO drops.** The thin creases `--ssao_downscale 4` can't
   resolve are exactly the static material detail a baked map carries — for one texture
   sample. So you could run SSAO *cheaper* and let the map do the fine work.
2. **Material grounding where SSAO is blind** — flat-ish surfaces with baked crevice detail.
3. **Near-free to add.** A sibling branch already loads normal maps from disk; AO is just
   another channel (or packed into a free channel). The G-buffer already does per-pixel
   texel fetch and carries tangent (§3).
4. **It flips the fusion verdict.** The kernel-fold above lost because it *added* an AO
   sample to the kernel. In a PBR kernel the ambient term is **already** multiplied by the
   AO map per pixel — SSAO rides that existing multiply for ~free, with no separate apply
   sweep. The fusion is worth it precisely when the kernel was going to do the multiply anyway.

Caveat: only helps if assets actually have baked AO maps — the greets meshes likely weren't
authored with AO bakes, so they'd need baking (or packing alongside the normal-map work).

To add a similar pass: copy the file's three-wave structure (compute → denoise →
apply), register flags in `FeatureFlags.def`, add the `.cpp` to `FDS/CMakeLists.txt`'s
`RENDER` group, declare the entry point near the other `void Render_*();` in `RENDER.CPP`,
and call it in `renderFrame` at the right point in §2.

---

## 10. Quick reference — key files

| Concern | File |
|---|---|
| Frame orchestration | `FDS/RENDER/RENDER.CPP` (`renderFrame`) |
| Deferred lighting kernel | `FDS/RENDER/DeferredSurfaceKernel.cpp` |
| Light lists / culling | `FDS/RENDER/DeferredLightLists.cpp` |
| Fog / froxels | `FDS/RENDER/DeferredFastFog.cpp` |
| Volumetric cones / halos | `FDS/RENDER/DeferredVolumetric.cpp` |
| SSAO | `FDS/RENDER/DeferredSSAO.cpp` |
| Shared deferred ctx/types | `FDS/RENDER/DeferredCommon.h` |
| Shadow sampling | `FDS/RENDER/DeferredShadowSampling.h`, `Shadows.cpp` |
| HDR buffer + tonemap + bloom/lens | `FDS/RENDER/Hdr.{h,cpp}` |
| G-buffer struct + oct codec | `FDS/FILLERS/Mekalele.h` |
| Rasterizer (G-buffer fill) | `FDS/FILLERS/Mekalele.cpp`, `TheOtherBarry.h` |
| Runtime flags | `FDS/Base/FeatureFlags.{def,h,cpp}` |
| Camera/view globals | `FDS/Base/FDS_VARS.H` |
| Headless snapshots | `DEMO/Snapshot.cpp` |
| Env maps (padded cube faces) | `FDS/RENDER/EnvCube.h` (convention + trig-free lookup), `EnvBake.cpp` (deferred bake), `DEMO/CITY.CPP` (per-building bake) — see `docs/ENV_CUBEMAP_PLAN.md`; `--no-env_cube` = legacy equirect |
