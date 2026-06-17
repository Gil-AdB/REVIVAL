# Fountain HDR prototype — scope

Goal: prove out an HDR intermediate + tonemap on **one scene (fountain)** behind a
flag, to decide whether to roll HDR engine-wide. Fountain is the right testbed:
it's the most additive-heavy scene (lightning bolts, the transient flash, froxel
fog in-scatter, lens flares) and the deferred lighting kernel already does its
math in float, so the payoff and the cost both show up here first.

## Why (the symptoms we already pay for)

The engine fights 8-bit LDR in exactly the places HDR fixes:

- **`glowMax` soft-knee** (DeferredLighting.cpp:7171-7179, 7982-7990) — a
  hand-rolled compressor squeezing float in-scatter into 0..255. This IS a
  tonemap, applied per-pass instead of once at the end.
- **`add_saturated` blends** — additive bolts/flares/fog clip to white; bolts
  wash out over the flash.
- The deferred kernel **computes float, then discards the range** packing to
  8-bit `out[i]` (DeferredLighting.cpp:2186 etc.).

HDR removes these hacks rather than adding a feature.

## Not a bandwidth question

Profiling has repeatedly shown the renderer is **compute-bound, not
bandwidth-bound**. So:

- The only genuinely-new *compute* is the full-screen tonemap, which is cheap
  (Reinhard = one divide; ACES = a short polynomial) and tiles like every other
  pass.

### Buffer format: use f16 (RGBA16F), verified cheap on both targets

f16↔f32 conversion is a **single hardware instruction** on both architectures —
the earlier "use f32 to dodge conversion compute" worry was wrong:

- **arm64 / NEON** (baseline ARMv8, incl. Apple Silicon): `fcvtn` (f32→f16) /
  `fcvtl` (f16→f32), 4 lanes each. Verified by disasm: `vcvt_f16_f32` →
  `load; fcvtn; store`.
- **x86 / F16C** (every Intel since Ivy Bridge 2012, AMD since Jaguar):
  `VCVTPS2PH` / `VCVTPH2PS`, 8 lanes (`_mm256_cvtps_ph` / `_mm256_cvtph_ps`).

So prefer **RGBA16F (8 B/px)** — GPU-standard HDR format, half the footprint of
f32, and precision is ample for display-range radiance + bloom (it's exactly
what hardware HDR render targets use).

**One simde gotcha (verified):** on arm64, simde's `_mm256_cvtps_ph` (down)
lowers cleanly to `fcvtn`/`fcvtn2`, but `_mm256_cvtph_ps` (up) does **not** — it
falls to a scalar/`bfi` path (no `fcvtl`, "loop not vectorized"). The raw NEON
`vcvt_f32_f16` emits `fcvtl` fine. So wrap the up-convert in a tiny native helper
(NEON on arm64, `_mm256_cvtph_ps` on x86) — same pattern as the existing
simde-arm64 patches (`approx_rsqrt`, `any_lane_set`, the mullo/testz fixes). The
down-convert can go straight through simde.

## Design

Add a float32 HDR buffer parallel to `vpage`. The deferred path writes **scene-
referred radiance** into HDR (no `glowMax`, no saturation). A final **tonemap
pass** maps HDR→`vpage` (8-bit BGRA, the SDL-locked buffer). UI/text/overlays
stay where they are — drawn into `vpage` *after* tonemap, untonemapped.

```
RenderSkyCube  ─┐
deferred opaque ├─► HDR (float32 RGBA, scene-referred)
fog / glow      │
transparent     │
additive overlays (bolt/flares) ─┘
                      │
                 TONEMAP + sRGB  ─► vpage (8-bit)
                      │
                 UI / text overlays (8-bit, on top)
                      │
                    Flip
```

## Phases (evaluate after Phase 1)

### Phase 0 — plumbing + tonemap + flag  (~120 lines, low risk)
- `RenderTarget` (FDS/Base/RenderTarget.h): add `float *hdr = nullptr;` (RGBA32F,
  same xres/yres, stride = xres*4 floats).
- Allocate/free the HDR buffer alongside the vpage resize path; zero it at frame
  top (it's the accumulation target; replaces the implicit vpage clear for the
  scene area).
- `FeatureFlags.def`: `FDS_FLAG_BOOL(hdr, "FDS_HDR", 0, "atmos", "...")`.
- New `Render_TonemapToVPage(const RenderTarget&)` — tiled (mirror Render()'s 6×4
  job model), per pixel: read `hdr[i]`, apply tonemap (start with ACES-fitted),
  encode sRGB, pack BGRA → `vpage[i]`. SIMD: 8 px × 4 float. ~60 lines.
- Insertion: called once after all scene passes, **before** the UI/text overlays
  in the fountain tick (so text isn't tonemapped). Gated on `hdr()`.

### Phase 1 — deferred opaque + fog + xpar + sky → HDR  (~250-400 lines, hot path)
This is where the payoff (and the work) is.
- **Opaque lit write** (DeferredLighting.cpp:2186, scalar + any SIMD store
  variant): write raw float radiance to `hdr[i]` instead of clamped uint32.
- **Fog fold** (2460-2495): drop the 0..255 clamp; accumulate raw. **Delete
  `glowMax`** (7177, 7987) — radiance is unbounded now; the tonemap handles it.
- **Transparent composite** (2522) + **water** (4009): blend in HDR (read/write
  float). `XparBlendAlpha` lerp carries over unchanged (it's a ratio).
- **Sky**: the forward skycube is LDR/display-referred. Decision: simplest is to
  **load it into HDR as linear** (8-bit→float) so it tonemaps with everything;
  expect to re-tune sky brightness. (Alt: composite sky post-tonemap — avoids
  re-tune but the sky then can't receive bloom; reject for now.)
- Audit all `out[i]=` sites from the grep list; viz/debug paths can keep writing
  vpage directly (they're not scene color) as long as they run post-tonemap.

### Phase 2 — additive overlays (bolt / flares / additive vortex) → HDR  (~150-250 lines)
- These go through `TheOtherBarry` (8-bit `add_saturated`) and the omni/flare
  fillers, which assume a uint32 framebuffer. Two options:
  - (a) HDR-target variants of the additive fillers (float add into `hdr`), or
  - (b) keep them 8-bit into a **separate additive layer** added into HDR before
    tonemap.
- (b) is the smaller change and isolates `TheOtherBarry` from HDR; (a) is
  "correct" long-term. Prototype with (b).
- This is what actually fixes **bolt/flare washout over the flash**.

### Phase 3 — linear color space  (~100 lines + re-tune)
- sRGB→linear LUT on texture load (BPPConvert / Load_Texture path).
- All lighting/blend math is then linear (mostly already is, just fed wrong
  inputs today).
- Tonemap does linear→sRGB on output (fold into the encode).
- Pairs naturally with HDR (linear in 8-bit storage would band; HDR removes
  that). Biggest *correctness* win, smallest *visible* win until combined.

## The real cost: re-tuning, not code

Every fountain brightness/ambient/intensity/`glowMax`/flash/fog-density value was
hand-tuned **against 8-bit clipping**. Under HDR+tonemap they all shift. Budget
the re-tune of the fountain as part of Phase 1, and treat "does it look better
after re-tune" as the go/no-go for rolling HDR to city/greets/etc.

## Validation

- Headless A/B via the fountain snapshot harness (`--snapshot=fountain@t=...`),
  `--hdr` on vs off, at flash + bolt + portal moments.
- Specifically confirm: flash no longer flat-whites the fog; bolts stay crisp
  over the flash (Phase 2); flares roll off instead of clipping.
- Watch the tonemap pass cost in the profiler (new full-screen compute); confirm
  it tiles and stays in the single-digit-ms range.

## Open decisions

1. Tonemap curve: ACES-fitted (filmic, contrasty) vs Reinhard (cheap, flat).
   Start ACES; expose exposure as a flag for tuning.
2. Sky handling (linear-and-tonemapped vs pass-through) — see Phase 1.
3. Phase 2 (a) vs (b) — HDR fillers vs separate additive layer.
4. Whether to keep `glowMax` as a fallback path when `--hdr` is off (yes — leave
   the LDR path intact; HDR is purely additive behind the flag).

## Rough size

Phase 0+1 (enough to judge the fog/flash payoff): **~400-500 lines**, fountain-
flagged, hot-kernel writes touched but the opaque *rasterizer* (Mekalele
G-buffer) untouched. Phase 2 adds bolt/flare fix. Phase 3 is correctness.

---

## STATUS — Phase 0/1 DONE + auto-tune (2026-06-17)

Shipped (gated on `--hdr`, off by default; render-gate ALL PASS flag-off):
- **Phase 0** (711db6d): `FDS/RENDER/Hdr.{h,cpp}` — f32 BGR `g_hdrBuf` + tonemap; flags `hdr`/`hdr_exposure`/`hdr_white`.
- **Phase 1** (dd5060c): froxel composite (`Froxel_CompositeTile`) writes unclamped lit+fog → `g_hdrBuf`; `glowMax` forced 0 in HDR; tonemap in `renderFrame` after the fog. Black-scene fix (aed13e7): `g_hdrActive` guard so the tonemap no-ops when the froxel composite didn't run.
- **Tonemap** (0df8c61): ACES (Narkowicz); `hdr_white` repurposed as post-tonemap chroma scale.
- **Auto-tune** (3213e35): `hdr_glow_scale` (default 0.25) scales the glowMax-cap-tuned inputs (`fast_fog_inscatter`, `bolt_flash_peak`, `cone_strength`) in HDR so the *original* flags work under `--hdr`.

Tuned fountain cmdline: original glowMax flags + `--hdr --hdr_exposure=0.9 --hdr_glow_scale=0.12 --fast_fog_density=6` (density 14→6 kills the milky soup — that's *extinction*, unrelated to HDR/glowMax).

Key findings: the **portal is additive-forward (pre-fog) → already HDR** via the composite read; **soupiness = fog density**, not glow; glowMax was already a per-slice tonemap, so HDR's win needed re-tuning the cap-leaning inputs (done via `hdr_glow_scale`).

## Phase 2 — overlay HDR reorg (IN PROGRESS, the remaining work)

Goal: bolt/cones/sprites/water all composite LDR over the tonemapped base today (~1.3% pure-white / ~4.8% R clipping in the bolt+flash frame). Route them through `g_hdrBuf` so they bloom/roll off.

Mechanism: **walk the tonemap to the end** (currently `renderFrame` right after the fog) and redirect each overlay's `out[i]`→VPage write to `g_hdrBuf[i*4]` (float), gated on `g_hdrActive`. All edits are `--hdr`-gated, so the byte-gate (flag-off) stays valid; verify the HDR path by fountain/greets snapshot.

Pipeline order + redirect sites (do pass-by-pass, walking the tonemap down, commit each):
1. **xpar** — `Render_DeferredTransparentLighting_Tile<Front/Back>` (DeferredSurfaceKernel.cpp: water blend ~1253, general/`XparBlendAlpha` ~1634) **and** `Render_DeferredLighting_TileFill` (~2696, water) — the latter is the `--deferred-quarter` path (active in the user's cmdline). Read dst from `g_hdrBuf` (float), write `g_hdrBuf`.
2. **sprites** — `TBR_Render` / sprite filler (RENDER.CPP ~674) → `g_hdrBuf`.
3. **cones** — `Render_VolumetricCones` (DeferredVolumetric) → `g_hdrBuf`.
4. **halos** — `Render_OmniHalos` → `g_hdrBuf`.
5. **rain** — `Render_ScreenSpaceRain` → `g_hdrBuf`.
6. **bolt** — `DrawActiveBolts` (FOUNTAIN tick): `TheOtherBarry<ADDITIVE>` ribbon/quad + `Lightning_Line` core → float-add into `g_hdrBuf`. This moves the tonemap into the fountain tick (after the bolt); needs a defer flag so `renderFrame` skips its tonemap when the scene will do it post-overlay.

Note: portal already HDR (skip). Most passes are float-internal (cones/halos/xpar) so the redirect is an output swap; the bolt/sprites integer rasterizers need a float-accumulate at their store site (additive: `g_hdrBuf[i] += src`).
