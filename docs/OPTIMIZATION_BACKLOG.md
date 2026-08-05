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

- **B5 per-face screen-bbox tile-walk pre-reject — DONE (2026-08-02, S2,
  commit 9b6d70d, `--tile_bbox_cull` default ON).** Each face's projected
  screen bbox (int16, floor/ceil+1px, from the face's own A/B/C PX/PY) is
  stamped into its `FListEntry` at FList-build time (Transform.cpp push); the
  tile walk (`RenderInnerMekalele`/`RenderInner`) 4-compare-rejects a face whose
  bbox misses the tile rect BEFORE the Face deref — a rejected face costs only
  the sequential FListEntry read (skips the 3 scattered Vertex flag loads + the
  clipper entry). PURE reject (the clipper already clips to the tile →
  byte-identical); near-plane-straddling faces (any vert behind nearZ) keep a
  cover-all sentinel and are never rejected. **Measured (t=5780, 1080p, threaded,
  40-iter p50, cull OFF→ON):** edge-displaced greets (86.6k faces) frame
  99.3→86.85 (−12.5), RNDR 63.66→51.63 (−12.0); t=2145 108.8→100.0 (−8.8);
  flags-off greets 47.5→47.0 (−0.5); city 95.1→94.1 (−1.0). Byte-null: city
  37e62845 + fountain 51fff7cd exact, render_gate 3/3, displaced-greets
  stable-pixel 1px/2.07M (greets race), wasm links. TRAP found: `frame->PX` is
  NOT populated by `VertexFrame_DumpFromAoS` (only PY), and conetest's giant
  quad has unpopulated `*_idx` — hence the fill reads AoS `F->A->PX` directly.
- **B5 shadow-bake face cull — SUPERSEDED by S1 offscreen proxy** (commit
  376f826, `--greets_shadow_proxy`, opt-in). Instead of per-face rejecting
  displaced faces in the shadow raster, the whole displaced detail is
  main-camera-only (Face_MainOnly) and a FLAT ~226-face proxy casts/reflects in
  every offscreen pass. **Measured: BAKE 27.3→21.5 (−5.8), frame 92.8→88.1
  (−4.7) at t=5780.** The win is BOUNDED — the shadow cube-face cull already
  limited per-frame wall rastering, and mixed chunks (rooms+siling) still pay
  Phase-A transform of ~22k displaced verts (only 151 pure-displaced chunks /
  59k faces are fully Tri_NoShadowCast'd). Default OFF: the flat caster's
  shadows differ from the displaced walls' on the looked-at wall (~1% px,
  maxD 142 — NOT invisible, a look call) and a flat caster carries no relief so
  it does NOT fix the reported light-bleeding.

- **S3 mesh-level AABB-vs-frustum cull — MEASURED, NOT WORTH IT (2026-08-02).**
  XFRM (the per-vertex transform, where a pre-transform AABB reject would save
  work) is 0.70 ms flags-off greets / 2.31 ms city / 7.8 ms edge-displaced
  greets — all tiny vs the RNDR (50-80 ms) + BAKE (20-30 ms) elephants. The
  existing bsphere cull + the greets Piramid chunk split already reject most
  off-screen geometry (XFRM would be an order of magnitude higher if they
  weren't firing — they are). An AABB is a tighter bound but its ceiling is a
  fraction of XFRM (sub-ms). At the heavy displaced pose the walls are ON-screen
  (that's why they're displaced) so an AABB wouldn't cull them either.
  Foundation-F AABBs stay consumed only by the env overlay; skip.

- **S4(a) fan↔edge seam holes — SPEC (not landed; risk vs budget).** The
  cross-patch heal (`MeshOps.cpp:2446-2490`) only REPOSITIONS the finer side's
  verts onto the coarser (anchor) polyline; it never INSERTS a vert on the finer
  side at an anchor kink it lacks. Fan(i/2^L params)↔edge(groove params)
  junctions have no subset relation → the fan chord bypasses the edge's groove
  kink → hairline hole. FIX = union the two sides' param lists at TRIANGULATION
  time (both sides emit `edgeVert` at every union param → welded, heal becomes a
  no-op) — a two-phase change to the tessellator (register-all-params, then
  re-emit), too invasive to retrofit safely late. First step: a diagnostic
  counting sides where neither param list ⊆ the other (the true un-healable
  count) next to the `%d T-junction pins` log.

- **S4(b) stone light-bleeding — SPEC (AO-on-direct; S1 does NOT fix it).**
  Bleeding = DIRECT disco light on mortar that the single-shadow-id collapse
  (acne fix) left un-self-shadowed. A FLAT proxy has no relief so it cannot
  restore per-block mortar self-shadow (the coordinator's S1-fixes-bleeding
  premise is geometrically wrong — verified). The map's grooves ARE the missing
  occlusion. AO already exists (rooms/floor carry it in albedo-alpha,
  `Mat_AoInAlpha`) but is applied to AMBIENT ONLY
  (`DeferredSurfaceKernel.cpp:1856-1874`). FIX = behind a default-off greets flag,
  for `Mat_AoInAlpha` displaced mats, move the AO multiply from ambient-only to
  the FINAL (ambient+direct) color after the light loop → static, acne-free
  groove darkening under direct light. Hot-kernel change (scalar path covers
  greets); flag-gated for byte-null; strength is a user look call. Tuning-only
  fallback: raise `ao_map_strength` (deepens ambient grooves, won't fully stop
  the direct leak).

- **S5 chunk-level LOD (flat+POM near/edge-displaced far) — DESIGN NOTE.**
  Largely SUBSUMED by S1: the offscreen proxy already IS the flat LOD for every
  non-main view. A camera-distance main-pass LOD would bake both meshes per
  chunk and swap by distance via the existing chunk machinery — but the S1
  Face_MainOnly/Tri_OffscreenProxy split + the proxy mesh are exactly the dual
  representation an LOD needs; extending it to swap in the MAIN pass by distance
  is the remaining step. Low priority: RNDR is pixel-bound at these poses (the
  displaced faces add little coverage — B2's ~2-2.8 µs/face is fixed cost, which
  S2 already reclaimed), so a main-pass geometric LOD buys little beyond S2+S1.

## Geometry front-end (XFRM) — measured 2026-08-05, docs/SOA_VERTEX_REFACTOR.md

> **RE-MEASURED 2026-08-06 — THIS SECTION IS CLOSED AS A PERF TARGET.** With
> tessellation retired and `9d`'s faceless-mesh cull landed, the WHOLE geometry
> front end is **≈1.2–2.6 ms of a ~79 ms greets frame (1.5–3.3 %)**: main-view
> `Transform_Objects` **0.449–0.468 ms** min (SETUP 0.004 / VERT 0.244–0.257 /
> SOA 0.001 / FACE 0.176–0.185) at 49 447 verts, SHADOW phase A 0.74–2.07 ms
> wall across both bake calls, OFFSCREEN ~1.18 core-ms. The 7.92 ms baseline
> quoted below is `--greets_displace`, which is retired geometry.
> **One thing did land from this pass:** the per-light shadow depth/polyId
> `std::fill` ran SERIALLY on the tick thread inside the window `--shadow_prof`
> calls `xform=` — 10.50 MB / 0.19–0.40 core-ms per DynMeshes bake, i.e. MOST of
> that bucket was memset, not transform. Now cleared on the owning phase-A
> worker (byte-null at every gate). Census: `-DFDS_SHADOW_CLEAR_CENSUS`
> → `[SHADOW-CLEAR]`.
> **Phase 5's ceiling is also half what the doc claims** (−26 %, not −45 %, once
> the 13 extra SoA write streams are counted) unless the outputs are laid out as
> ONE interleaved 64-byte-per-vertex array instead of 13 SoA arrays — see the
> 2026-08-06 section of docs/SOA_VERTEX_REFACTOR.md for the corrected design,
> the verified `sizeof(Vertex)`=140 → 68 target, and the full consumer inventory
> (the migration surface is 11 files, incl. `Reflected_Transform` and FOUNTAIN).
> Prize for the whole refactor: ~0.15–0.4 ms (0.2–0.5 % of frame). Do it for
> cleanliness if at all, not for perf.

Instrument: `--xfrm_prof=N` + `--xfrm_ablate=<mask>` (both default OFF, byte-null).
Baseline at greets t=5780 `--greets_displace`, per-frame min: main-view
`Transform_Objects` 7.92 ms = VERT 4.01 + SOA 2.40 + FACE 1.45.

- **DONE — the Phase-1 AoS→SoA dual-write sweep, 2.40 ms.** `--xfrm_soa_inline`
  (now default ON) moves the SoA store into the per-vertex loops. Measured
  7.93 → 5.96 ms (−1.97, −25 %) at t=5780; bit-exact (city/fountain pins exact,
  chase 7 poses + greets t=1588/t=5780 byte-identical, `--soa-verify` clean).
- **The 958 k verts are mostly MIRROR CLONES, not the wall.** The displaced
  Piramid is 261,768 verts; the main view transforms 958,204, because
  `GreetsMirror` clones the whole scene per mirror (534,356 verts each for
  'teleporter' and 'P_TEXT.JPG#6') as ordinary meshes gated only by
  `HTrack_Visible`. **This is now the single biggest front-end lever** — bigger
  than anything left inside `Transform_Objects` — and it lives in
  `FDS/RENDER/GreetsMirror.cpp`: per-clone frustum/visible-panel culling, or a
  decimated clone of the displaced stone (the reflection does not need
  block-level relief). **CEILINGS MEASURED 2026-08-05** — see
  `docs/VISIBILITY_PLAN.md` §8, instruments `--mirror_cull_census[_cell]` +
  `--xfrm_pass_prof` (all default OFF). Exactly ONE clone is active at the wall
  poses = 56 % of main-view transformed verts; mirror RTT passes DO NOT run
  (`--mirror_rtt` defaults 0) and hide clones anyway, so this is purely a
  main-view cost. A per-source-mesh split saturates at ~40 %; a SPATIAL split
  of the clone at ~8 world units (103 sub-meshes) culls **63 % at wall poses /
  89 % at a panel pose** — but only against the MIRROR WINDOW (0.04–3.9 % of
  screen), not the frustum (2–20 %). Estimated ~2 ms off a 5.9 ms main-view
  `Transform_Objects`. NOT built: a spatial split breaks `UpdateMirror`'s
  contiguous-`ClonedMeshRange` invariant and every `m.cloneMesh` consumer
  (RTT/shatter/editor/envbake) — see §8e. **Cheaper first move: 49 % of the
  clone is the displaced Piramid; clone the S1 flat `stone_shadow_proxy`
  instead (~226 faces vs 87,256).** Look call on reflection fidelity.
- **A visibility HIERARCHY (BVH/octree) is REFUTED with numbers (§8c).** ~9,150
  mesh-level tests/frame across ~40 passes, ~7,690 of them rejections, at a
  measured ~55–61 ns per rejected sweep = **0.45 core-ms/frame** total prize,
  ~90 % of it inside 12-way-threaded shadow passes. A hierarchy also cannot
  reduce transformed verts at mesh-sized leaves, and finer leaves measured a net
  LOSS (verts −3.3 %, XFRM +1.08 ms).
- **Phase 5 of the SoA refactor is a PERF item now, not just cleanliness.**
  `Vertex` is pack(1) 140 B and the per-frame loop touches fields spanning
  offsets 4..123 — every cache line. Ablating 2 of the 3 per-vertex mat-vecs
  (34 % of the struct) buys only 8.7 % of VERT: the loop is line-bound, not
  arithmetic-bound. Moving the per-frame-written outputs out of the AoS struct
  shrinks the stride the transform walks; ceiling ~45 % of VERT. (Corollary:
  Phase 2 / Vec8f stays parked — widening lanes cannot help a stride problem.)
- **Per-face visibility test = ~73 % of the FACE bucket** (t=6097: 0.322 of
  0.395 ms). `Face::VisibilityFlagsAll()` is `A->Flags & B->Flags & C->Flags` —
  three chases into 140-byte `Vertex` structs; the tile-bbox stamp chases the
  same three again. Reading `Flags`/`PX`/`PY` from the VertexFrame SoA arrays
  (4-byte stride) is the obvious fix, BUT it requires `F->A/B/C_idx` to be
  trustworthy on every mesh and the tile-bbox comment in Transform.cpp records
  meshes where they are not (the conetest quad) — a wrong bbox DROPS a face
  where a wrong SortZ was harmless. Needs a per-mesh "indices stamped" invariant
  first.
- **Per-chunk normal cones** (bulk-accept / bulk-reject a chunk's faces without
  the per-face dot) — still unmeasured. Ceiling is bounded by the FACE bucket
  (1.45 ms at t=5780, 0.40 at t=6097) and it cannot remove the SortZ/FList work
  for accepted faces, so expect a few tenths of a ms. Rank below the two items
  above.

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
- **`--greets_displace` at t=6097 is run-to-run NONDETERMINISTIC** (measured
  2026-08-05): 6 sequential runs → 6 distinct color-PPM hashes, with every new
  flag OFF, while t=5780 in the same runs was byte-stable 6/6. So the "greets is
  deterministic again" re-pin (f4e81e9) is scoped to the NON-displaced pin
  recipe; the displaced path has its own defect at that pose. t=6097 cannot be
  used as a byte gate until this is root-caused. Not investigated — found while
  gating the XFRM work.

## Architecture cleanup — retire the ::mirUV material split (QUEUED 2026-08-05)

**What.** Replace the per-MATERIAL UV-handedness clone with a per-FACE
handedness bit, and compute `B = handedness·(N×T)` from it everywhere.

**Why now.** `GreetsFixBitangentHandedness` (DEMO/GREETS.CPP:1254) splits every
face with a negative UV determinant onto a `<name>::mirUV` clone carrying
`TbnHandedness = -1`, because the DEFERRED kernel is per-pixel and the G-buffer
has no channel for handedness — a material clone was the only path available.
Handedness is a property of a TRIANGLE's UV winding, and the determinant is
already computed at the split site; it is simply thrown into a material clone
instead of being stored. The split has caused real damage all session:

- **The parallax march never applied it** — `Mekalele.h:1462` builds
  `B = N×T` unconditionally while `DeferredSurfaceKernel.cpp` applies the sign
  in 5 places and `LightmapBake.cpp:793` applies it. With 41.8% of greets charts
  mirrored (S1d-1 census), the march walked the height field in the OPPOSITE V
  direction on those faces — the user's "texture edge moving half the face"
  swim. Found 2026-08-05 only because the user observed that deeper faces at
  STEEPER grazing angles did NOT swim, which falsified the angle-based theory.
- **`floor` has ZERO faces under its own name post-init** (they all moved to
  `floor::mirUV`), which silently gave the floor no shell at all and cost an
  agent hours.
- **`::mirUV` clones ALIAS their base material's shell tables** — naive frees
  are a use-after-free (hit during the editor rebuild work).
- It inflates material counts through the seam census and amplitude audits.

**Where.** Store the sign in the `Face_*` flag bits (a free bit, not a new
field). Do NOT put it per-vertex: `Vertex` is pack(1) 140 B and the XFRM profile
measured the transform loop CACHE-LINE-BOUND, so widening it costs ms. Stamp at
the determinant computation in `GreetsFixBitangentHandedness`; consumers are
`Mekalele.h` (march), `DeferredSurfaceKernel.cpp` (×5), `LightmapBake.cpp`.
The deferred kernel needs the bit reaching per-pixel — that is the hard part and
the reason the split exists; check whether a G-buffer bit is available or
whether the material lookup can carry it without a clone.

**Sequencing.** AFTER the immediate march fix (thread `Material::TbnHandedness`
into the raster context) — that unblocks the swim and is small. This entry is
the follow-up that fixes the CLASS rather than the instance.

**Expected benefit.** Removes a whole defect class; no measured perf claim.

## Offscreen geometry — what is LEFT after the faceless-mesh skip (2026-08-06)

Context: `docs/VISIBILITY_PLAN.md` §9. The faceless retired-Piramid skip
(`799c808`) took SHADOW 540 706 → 175 594 verts/frame at t=5743 and is DONE.
Ranked by share of what remains, measured with `--xfrm_pass_mesh_prof`:

- **`Hull.lwo` (the robot) — 82 800 verts/frame, 47.2 % of the post-fix shadow
  front end.** TODO. 2 400 faces / 7 200 verts, transformed in ~11.5 of 29
  shadow calls per frame. Levers: a shadow-caster LOD, or tightening the
  per-light mesh cull so fewer of the 29 calls keep it. Both are look-neutral
  inside a shadow map (silhouette-only consumer), so this is a perf call, not a
  look call — unlike the wall proxy below. **Measure before building:** the
  whole SHADOW front end is ~10.7 core-ms/frame post-fix, so the ceiling here is
  ~5 core-ms, i.e. sub-ms of wall clock at the pool's speedup.
- **`Piramid.lwo:cNN` chunks — 80 635 verts/frame, 45.9 %.** PARKED. Already
  chunked; VISIBILITY_PLAN §7b measured finer chunking a net LOSS.
- **robot legs — 11 403 verts/frame, 6.5 %.** TODO-if-#1-pays (same lever).
- **~~wall casters (flat proxy for `rooms`/`floor`/`siling`)~~ — PARKED, MEASURED
  NOT WORTH IT.** `rooms` + `rooms::mirUV` are 2 506 of 540 706 shadow verts
  (0.46 %); with `floor*` and `siling` ~4 000 (0.74 %), = 2.3 % post-fix.
  `--greets_shadow_proxy` was sized against ~81 k **displaced** faces; with
  tessellation retired the shadow bake already rasterises the ~226-face flat
  surface the proxy would substitute, so that win is already banked. Do not
  re-propose without a new measurement.

**Separate LOOK call, not actioned:** mirror clones are hidden inside the
mirror RTT scope but **not** inside the env/SH probe bakes — 110 030 of 309 740
OFFSCREEN verts/frame pre-fix (~68 540 post-fix) are clone meshes transformed
for probes. Whether a probe should see a reflection at all is the user's call;
hiding them there is a one-line scope change worth ~68 k verts/frame.

## How this list is maintained
Add an entry the moment an optimization is deferred (with: what, why deferred,
where in code, expected cost/benefit). Mark DONE with the commit + measured
result. SESSION_STATE "Queued next" points here; the memory
`optimization-backlog` points here too.
