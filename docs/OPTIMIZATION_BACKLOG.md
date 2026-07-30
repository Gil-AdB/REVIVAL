# OPTIMIZATION BACKLOG

Tracked list of deferred optimizations + measured-quality upgrades so they
don't get lost. Rule for this CPU software renderer (measured): it is
**gather-bound**, not FLOP-bound — but "texture reads are expensive" is NOT a
safe assumption (the env-reflection tap measured ~free). So: **measure each
change** (bench `--snapshot=<scene>@t=<T>@iters=<N>` → `mean ms/iter`,
interleave flag off/on ≥6×, take mins vs the noise floor). Everything here is
behind a default-off flag until measured + look-approved.

Status keys: TODO · IN-PROGRESS · DONE · PARKED (measured not-worth / blocked).

## PBR quality series (incremental, one at a time, user reviews each)
Discussion: engine PBR compositing vs canonical (SESSION_STATE / chat 2026-07-13).
The engine is standard in the big decisions (additive diffuse+specular, GGX,
Schlick, metalness→F0, roughness-mip reflection); it simplifies in 3 places —
each is a candidate below.

**SERIES COMPLETE (2026-07-14):** all four increments (#1 analytic split-sum
env-BRDF, #2 SH irradiance ambient, #3 (1−F) diffuse energy, #4 multi-scatter
compensation) have LANDED on fog-wt, each default-OFF and each measured
effectively free (within the frame noise floor). All four await user
look-approval before any default flip. The env-BRDF LUT and diffuse-cubemap
items below stay PARKED.

- **#1 analytic split-sum env-BRDF** — DONE (e0640fe, flag `env_brdf_analytic`,
  default OFF). Measured +0.72 ms on greets = within noise → effectively free.
  Retires the `f90=1-rough` hack. Awaiting user look-approval to default ON.
- **#2 SH irradiance ambient** — DONE (d29302a, flag `sh_ambient`, default
  OFF). Replaces the flat `Sc->Ambient` constant with 9-coeff L2 RGB SH
  irradiance evaluated per-pixel along the (post normal-map) shading normal
  (~9 FMA/channel, no gather; clamped >=0). `SHAmbient_EnsureBaked`
  (EnvBake.cpp) projects a scene-center 32²×6 env cube — rendered through the
  same deferred cube-face path the env-reflection probes use — into 27 floats,
  A_l/π folded in so a uniform env evaluates back to its own colour
  (magnitude-comparable to the flat ambient). Injected at all three opaque
  kernel ambient sites (scalar wave-1 = greets; OuterVec vec-fill = city,
  covers its scalar fallback via `lane_ambB`; TileFill quarter/checker).
  Transparent kernel keeps flat ambient (out of scope). **Measured (arm64,
  1920×1080, whole-frame, 8 interleaved OFF/ON rounds, min-of-each):**
  greets scalar Δ = **+0.84 ms** (min 73.53→74.38; noise floor 6.3 ms) →
  within noise = **effectively free**; city OuterVec Δ = **+3.47 ms** (min
  103.26→106.73; noise floor 9.8 ms; city renders two deferred passes and the
  OuterVec SH is a per-lane *scalar* loop, hence the larger — still
  sub-noise-floor — delta). Flag-OFF byte-identical: city `37e62845…`,
  fountain `51fff7cd…`. A/B: matte pillars/walls gain directional 3D form
  (flat dead silhouette OFF → shaped fill ON); heavily-lit/emissive regions
  barely move (ambient is a small fraction there, HDR-compressed). Awaiting
  user look-approval to default ON. NB the one-shot bake renders the scene →
  nondeterministic on greets, so md5 pins must gate with the flag OFF.
- **#3 (1−F) diffuse energy conservation** — DONE (ccc0229, flag
  `diffuse_energy`, default OFF). Scales the deferred DIFFUSE accumulator by
  `(1-fres)` at the combine, where `fres` is the SAME per-pixel Schlick Fresnel
  the env-specular reflection already computes. Light reflected specularly (F)
  can't also diffuse; the engine scaled diffuse by `(1-metalness)` but skipped
  `(1-F)`, double-counting at grazing (full diffuse AND a strong Fresnel
  reflection). `EnvSpecComposeScalar` + `EnvComposeCityVec8` now expose `fres`
  via an optional out-param; wired at ALL opaque env-compose sites — scalar
  wave-1 (greets/fountain), TileFill, and OuterVec (both scalar-fallback lanes
  multiply the float diffuse; the vec fast-path uses an additive INTEGER
  correction `int(vf*(1-fres)) - int(vf)` so the flag-OFF path is byte-for-byte
  untouched). Transparent kernel carries no env term → out of scope (keeps full
  diffuse), same as #2. Only pixels with a reflection (Reflection>0 / metal)
  pay. **Measured (arm64, 1920×1080, city OuterVec, --snapshot=city@t=1961,
  iters=60, 8 interleaved OFF/ON rounds, min-of-each):** OFF min 100.344 ms →
  ON min 100.333 ms → Δ = **−0.011 ms** (ON marginally faster = noise; OFF
  noise floor min→max = 8.4 ms) → **within noise = effectively free** (it's ~4
  ALU ops per reflective pixel). Flag-OFF byte-identical: city
  `37e62845…`, fountain `51fff7cd…`. A/B (city glass, deterministic): reflective
  facades darken at grazing angles (blue windowed building + red facade go
  noticeably darker ON; 19.3% of pixels change, 100% darkened, mean luma −30.6,
  no pixel brightens), while the matte concrete pillar + emissive window-lights
  (no env term) stay byte-identical. Effect is PRONOUNCED on city because its
  glass has a high authored F0 (`city_env_f0=60`); on true dielectrics
  (F0≈0.04) it's the subtle grazing-only darkening the canonical BRDF intends.
  NB fountain shows zero change at its pin (its glass spheres are TRANSPARENT =
  no env term). Awaiting user look-approval to default ON.
- **#4 multi-scatter compensation** (Fdez-Agüera) — DONE (2718046, flag
  `pbr_multiscatter`, default OFF). The split-sum env-BRDF (#1) is single-scatter
  only — it drops the energy returned by repeated microfacet bounces, so rough
  metals read too dark. Adds it back from the SAME A,B `env_brdf_analytic`
  already computes in `EnvSpecComposeScalar` (a few ALU ops on reflective pixels
  only, NO new gather): `Ess=A+B` (single-scatter energy), `Favg=F0+(1-F0)/21`
  (avg Fresnel), `Fms=Favg*Ess/(1-Favg*(1-Ess))`, then `ek *= 1+Fms*(1-Ess)/Ess`
  (scales the SPECULAR energy only; the single-scatter Fresnel handed to #3 is
  left untouched). **DEPENDS ON `--env_brdf_analytic`** — it needs the A,B terms,
  so it's a NO-OP with that flag off (lives inside the analytic branch; the
  ad-hoc Schlick else-branch computes no A,B). `--env_brdf_analytic` already
  routes city glass off the OuterVec fast path to the scalar compose, so only
  `EnvSpecComposeScalar` needed wiring (4 call sites, 3 flag decls; no
  `EnvComposeCityVec8` touch). **Measured (arm64, 1920×1080, city@t=1961,
  iters=60, 10 interleaved OFF/ON rounds, `--env_brdf_analytic` as the baseline
  in BOTH to isolate #4 from #1, min-of-each):** OFF min 102.569 ms → ON min
  102.271 ms → Δ = **−0.298 ms** (ON marginally faster = noise/thermal, OFF ran
  first each round; noise floor max−min = 14.1 ms) → **within noise = effectively
  free**. Flag-OFF byte-identical: city `37e62845…`, fountain `51fff7cd…`
  (verified default AND `--pbr_multiscatter`-alone with `env_brdf_analytic` off).
  A/B (city glass, deterministic, `--env_brdf_analytic` baseline): reflective
  facades brighten — 19.2% of pixels change, **100% brighten / 0% darken**, mean
  +2.08 luma at the authored `city_env_gloss=24` (rough≈0.28, F0=0.6 → +10%
  specular), rising to +8.66 luma at a rougher `city_env_gloss=6` (the effect
  scales with roughness exactly as the compensation intends — characterized:
  F0=0.04 dielectric barely moves 1.00–1.05×, a true rough metal rough=0.8/F0=1.0
  → 1.79×, rough=1.0 → 2.22×). Non-reflective surfaces (concrete pillar,
  emissive window-lights) unchanged. NB city glass is a moderately-rough high-F0
  DIELECTRIC (metalM=0), not a true rough metal, so its brightening is modest;
  the effect is authored-material-dependent and pronounced only on rough metals.
  Awaiting user look-approval to default ON.
- **env-BRDF LUT (texture) vs analytic** — PARKED. The analytic (#1) is free +
  needs no memory; a real (F0·A+B) LUT is 1 cached tap. Only revisit if the
  analytic precision ever proves insufficient.
- **Diffuse irradiance as a cubemap** — PARKED (avoid). That's a per-pixel
  gather every lit pixel; #2 (SH) gets the same result as pure ALU.

## Micro-optimizations (only if a profile flags them)
- **env-BRDF `exp2`** — TODO-if-needed. `std::exp2(-9.28*ndv)` at
  DeferredSurfaceKernel.cpp ~1239 is scalar libm, and its result is usually
  CLAMPED away inside `std::min(rx*rx, exp2(...))` so full precision is wasted.
  The file already has a fast LUT-based log2/exp2 (used for `pow` ~2065) —
  reuse it (near-free), and it would vectorize into the `__m256` block so the
  city-glass AVX2 fast path (disabled today when the flag is on) could stay on.
  Measured free at greets scale, so LOW priority — do only if a profile shows it.

- **B5 per-face screen-bbox tile-walk pre-reject + shadow-bake face cull**
  (deferred 2026-07-31, ENVDYN plan workstream B). The B2 spike measured the
  per-face fixed cost of extra faces at ~2-2.8 µs/face SERIAL in RNDR (tile
  walk/clipper entry, `RenderInner.cpp:220-247` — no pre-reject exists) and a
  comparable BAKE share (the per-frame shadow bake re-rasters every subdivided
  face per light, camera-independent). At the shipped L=2 stone-subdiv density
  (+3.4k faces) the total is +2.3 ms threaded at the worst pose — declined as
  not material. Wanted only if L=3 (+14.2k faces = +11.5 ms threaded) is ever
  desired: then BOTH the walk pre-reject (4 compares vs the tile rect before
  clipper entry) AND a shadow-bake cull are needed — B5 alone can't rescue L=3
  because BAKE (+14-16 ms serial at L3) doesn't go through the tile walk.
  Numbers: docs/ENVDYN_DISPLACEMENT_PLAN.md B2 table.

## Perf (measured bottlenecks — from docs/PERF_STATE.md + the 15fps analysis)
The greets frame is ~2.5–3× a "generic deferred" frame; the fat is shadowing,
not shading. Biggest levers, in order:
- **Per-pixel CUBE-SHADOW taps ~32 ms** — the #1 cost (1.44M taps @ ~22 ns).
  `shadow_polyid_no_pcf` (single tap vs 4-tap PCF) saves ~9 ms already; fewer
  shadow-casting omnis; a cheaper cube-face select / better cache layout of the
  4 buffer streams. This is where the real fps is.
- **Dynamic-omni shadow re-bake ~12.5 ms** — re-rasterizing moving geometry to
  depth maps every frame. Cache/skip static portions.
- **Mirror RTT (teleporter, 2nd-order recursive)** — a full second scene
  render. Density/recursion knobs (`mirror_rtt_density`) are the lever.
- **Vectorize the general env-specular compose** (2026-07-31, user-queued).
  `EnvSpecComposeScalar` runs scalar per pixel for everything except the
  city-glass shape (`EnvComposeCityVec8` engages only for: uniform cube
  store across the 8-lane group + noParallax + cv-pull + no rough/metal
  maps + no env diagnostics + none of sphere-parallax/SSR/analytic-BRDF).
  Greets env surfaces are always scalar (per-material probes break the
  uniformity gate; AABB parallax; rough/metal maps). The math chain is the
  same one CityVec8 already vectorizes — per-lane stores become
  mask-selected, parallax correction is a few more vector ops; the
  gathers (face fetch) don't vectorize away regardless (ENV_NOFETCH
  attribution). MEASURE FIRST: only worth it if the env compose is a
  material slice of a greets frame vs the cube-shadow/cone elephants
  above.

## Correctness/determinism (blocks the gate, not just perf)
- **Greets env-bake non-determinism** — TODO, now URGENT-ish. Greets renders
  run-to-run distinct (4/4 by 2026-07-14, up from the old "~1-in-12 flip") —
  amplified by the user's new metallic materials (more env probes; env-bakes
  vary). This BREAKS the greets md5 gate (gate on city/fountain/pbrtest until
  fixed). Root-cause the bake ordering/threading nondeterminism. Related:
  measurement-tool-traps memory.
- **Editor metallic-import OOM** — IN-PROGRESS (targeted per-surface re-bake +
  capped editor bake res). Same env-probe subsystem as the determinism issue.

## How this list is maintained
Add an entry the moment an optimization is deferred (with: what, why deferred,
where in code, expected cost/benefit). Mark DONE with the commit + measured
result. SESSION_STATE "Queued next" points here; the memory
`optimization-backlog` points here too.
