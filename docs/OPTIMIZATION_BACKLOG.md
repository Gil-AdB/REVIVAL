# OPTIMIZATION BACKLOG

Tracked list of deferred optimizations + measured-quality upgrades so they
don't get lost. Rule for this CPU software renderer (measured): it is
**gather-bound**, not FLOP-bound — but "texture reads are expensive" is NOT a
safe assumption (the env-reflection tap measured ~free). So: **measure each
change** (bench `--snapshot=<scene>@t=<T>@iters=<N>` → `mean ms/iter`,
interleave flag off/on ≥6×, take mins vs the noise floor). Everything here is
behind a default-off flag until measured + look-approved.

Status keys: TODO · IN-PROGRESS · DONE · PARKED (measured not-worth / blocked).

## 2026-08-09 — HOT-STRUCT SWEEP: the front end is closed, the SHADOW-TAP planes are the item

Asked as *"trimming hot-struct bloat paid off once — is there more, here or in
other structs?"*. Three structs were audited against their consumer loops and
measured. **The winner is not `Vertex` and not `Face`; it is the four shadow-map
planes, and the mechanism is the same one (`cache LINES touched per access`).**

### 1. `Vertex` 140 → 68 (SoA Phase 5) — **PARKED, re-refuted with fresh numbers**

Full working in docs/SOA_VERTEX_REFACTOR.md (2026-08-09 section). Short version:
`VERT` is the only bucket it can touch and it is **0.345 ms at greets / 0.611 ms
p50 at city / 0.114–0.158 ms at chase**; the best case is ~40 % of that =
**0.24–0.31 % of frame**, for an 11-file refactor across two alternative transform
pipelines. chase is FACE-dominated (FACE 2.9–3.3× VERT), so it is the wrong lever
there by shape as well. Not attempted; do it for the one-writer contract if ever,
not for perf.

### 2. `Face` (162 B, pack(1)) — audited field-by-field; **layout work NOT indicated**

Full consumer audit done (every field × every compiled consumer, `FL.CPP` and
`3DS/WORLD.CPP` excluded as dead). Two structural facts worth keeping:

* **`alignof(Face) == 1` and the stride is 162, so `gcd(162,64) == 2` — a `Face`
  in `TriMesh::Faces[]` has 32 distinct cache-line PHASES.** Faces do not start on
  line boundaries; each spans 3 or 4 lines depending on index. **Any "put the hot
  fields in line 0" plan is defeated by the phase rotation** unless the struct is
  also padded to 128/192 or split into parallel arrays. This is the thing that
  makes `Face` unlike `Vertex`, and it should be checked before anyone proposes a
  regroup.
* **24 bytes of the struct are cold and sit in the middle of it.** `EU1..EV3`
  (offset 72) has exactly one active reader in the built tree —
  `FRUSTRUM.CPP:1065-1070`, *inside* `if (F->Flags & Face_Reflective)` — and
  downstream only `TheOtherBarry<…, TEXTURETEXTURE>` consumes it. `ReflectionTexture`
  (offset 112) is the same reflective-only gate. Together 32 B of a 162 B stride
  that the FList-build loop and Mekalele's per-face prologue both walk over for
  nothing. Evicting them to a side table indexed for reflective faces would take
  `Face` to 130 B.

**But the prize is bounded and was measured at ~zero.** The FACE bucket is only
0.254 ms (greets) / 0.539 ms (city), and §2 of the SoA doc already measured that
reducing lines-per-deref does not move it (the accesses are cache-resident). The
one live cross-core effect found in the audit was tested directly and **also
measured neutral** — see item 4. **Recommendation: do not spend effort on `Face`
layout.** The audit is recorded so the next person does not re-derive it.

### 3. Cube-shadow tap: PAIR-INTERLEAVE the four planes — **MEASURED, probe landed OFF, shippable version scoped**

**This is the item.** At greets t=5743 the cube tap is **10.28 ms of a 30.5 ms
`lighting-w1`** (`--prof_no_cube_tap`, min-of-3: 30.518 → 20.238; `--prof_no_lights`
→ 11.543 for scale). A PolyId tap needs a texel's `polyId` AND its `depth`, for the
static pair and the dynamic pair — **four separate `std::vector<uint16_t>` that at
greets' 512² sit 512 KB apart.** 32 bytes of useful data, gathered from 4 base
pointers over 2 PCF rows = **up to 8 cache lines (~512 B of line traffic) per tap**;
the static-lightmap composite path's dynamic-only tap still costs 4.

`--shadow_plane_pack` (LANDED, **default 0 = byte-null**) interleaves them:
`packSD[i] = depth[i] | (polyId[i] << 16)`, same for the dynamic pair. A texel's
id+z becomes ONE 32-bit load: **8 lines → 4, and 4 → 2. Total bytes resident are
unchanged**; only the grouping changes.

Measured, greets t=5743, 1920×1080, dummy drivers, `--deferred_prof=1`, per-frame
`wall_min`, min-of-6 over interleaved reps, on a QUIET box (1-min load 6.8–11.6):

| arm | `lighting-w1` | `renderFrame` |
|---|--:|--:|
| shipping, pack OFF | 27.750 | 43.475 |
| shipping, pack ON | **27.334** (−0.42) | 43.531 (+0.06, nil) |
| `--no-shadow_lightmap`, pack OFF | 28.145 | 43.859 |
| `--no-shadow_lightmap`, pack ON | **27.094** (−1.05) | **42.965** (−0.89) |

An earlier batch on a LOADED box (load 15–29) gave the same shape, larger:
shipping −0.66, `--no-shadow_lightmap` −1.69 on `lighting-w1`.

**Read the two arms together — that is the evidence.** `--no-shadow_lightmap`
forces every static omni onto the FULL four-plane tap (8 lines → 4) instead of the
lightmap + dynamic-only tap (4 → 2). Doubling the leverage roughly doubles the win,
which is what a lines-touched model predicts and what a noise artifact would not do
(5/6 reps favour ON in that arm, 4/6 in the shipping arm).

**Byte-null, certified differentially:** greets `778fa6ac…` 3/3, city `3cbe42b1…`,
fountain `8db68ccb…` — all EXACT, in all three arms (base binary, probe binary
flag-OFF, probe binary flag-ON). The packed tap is bit-identical by construction
(`CubeShadow_SamplePacked` replicates the linear tap's projection math verbatim).

**WHY IT SHIPS OFF, and what the real version is.** In probe form the packed planes
are a DERIVED COPY rebuilt after each bake, so it costs **+159 MB** (76 maps × 512²
× 4 B × 2) on top of the linear planes, plus the rebuild traffic. **The shippable
version makes the packed plane the SOURCE OF TRUTH** — replace the four
`std::vector<uint16_t>` with two `std::vector<uint32_t>` — which costs **zero extra
memory**, removes the rebuild entirely, and *also* speeds the bake (the shadow
rasterizer writes depth+polyId to the same texel: 2 arrays → 1). That is ~82 edit
sites across ~10 files, all mechanical, and the numbers above are its **lower
bound** (they are paid with the rebuild cost included). **Prize: ≥0.4 ms shipping /
≥1.0 ms with the full tap, MEASURED, not inferred.**

Orthogonal to `--shadow_swizzle`, which attacks the ROW-STRADDLE axis and measured
NEGATIVE in all 15 shapes (docs/ARCHITECTURE_NEXT.md). That experiment never
touched the parallel-plane axis.

### 4. `Face::LastMip` — a dead store from 12 workers into a shared line. **Removed; measured NEUTRAL.**

Found by the `Face` audit: `MiplevelClipper` wrote `F->LastMip` once per face
**per tile** from every tile worker, gated only on `g_mipLastMipWrite` (true), while
**both readers are gated on `mip_hysteresis > 0`, which is DEFAULT 0.** So at
shipping defaults it was a dead store — into byte 136 of a 162-byte `Face`, a line
shared with `ownerMirrorId` / `behindMirrorMask` / `A_idx..C_idx` / `frame`, all
read by other hot paths. Now gated on the same `mipHyst > 0` the readers use
(FRUSTRUM.CPP:784/867). Byte-null by construction.

**Measured NEUTRAL** — matched A/B pair built from ONE tree snapshot (the only
difference is this diff), greets t=5743, min-of-6, quiet box: `gbuffer` 5.286 →
5.201 (−0.085), `renderFrame` 43.630 → 43.440 (−0.19), `shadow-bake` 2.480 → 2.477.
All inside the run-to-run spread.

**That neutral result is itself a finding: it prices Face-tail cross-core line
contention at ~0 and is the direct evidence behind item 2's "do not do `Face`
layout work".** Kept because a provably dead store should not be executed, not
because it bought time.

### Also inventoried, not acted on (ranked by streamed bytes/frame)

| item | traffic/frame | residency | verdict |
|---|--:|---|---|
| cube-shadow taps (4 planes × 2×2 PCF + header) | ~1.0 GB of line traffic | 12.6 MB/cube omni ≫ LLC → DRAM | **item 3 above** |
| `Material` pointer chase | ~796 MB of accesses (6 lines/px: 1 matTable + 5 `Material`) | table ≤ 114 KB → **L2, not DRAM** | `sizeof(Material)` = 455 B, pack(1), no `static_assert`. Per-pixel fields are spread over lines 0/1/3/4/6 and the "hot fields on line 0" comment is **stale** — `TintR/G/B`, `SpecMul`, `AoStrength`, `Roughness/MetallicMap`, `Reflection`, `TbnHandedness` all drifted off it. No `matID` memo anywhere (every kernel re-chases per pixel). A 64-byte hot-fields record per matID (16 KB, L1-resident) collapses 5 lines → 1. **Not measured — candidate, but it is a latency item, not a bandwidth one.** |
| texture + aux-map point fetches | 130–660 MB | mip chains → DRAM | already measured: the albedo gather is worth only 0.71 ms (PERF_STATE) |
| `g_hdrBuf` | 166–232 MB (10–14 sweeps × 16.59 MB) | > LLC | `hdrf` is already `__fp16` on arm64 (8 B/px, not the 16 B PERF_STATE still quotes) |
| opaque G-buffer sweep | 35–44 MB read | sequential | 17 B/px unconditional from **6 separate arrays** = 9 concurrent streams/worker |
| `Omni` | 60 KB | trivial | `sizeof(Omni)` = **515 B**, not the "[256 Bytes]" its comment claims, and **302 B of it is a `Vertex` + a `Face`** the lighting path never reads. Pure list-walk pollution; harmless at 117 lights. |
| `TileLights` | 1.55 MiB resident | L1 per tile | `sizeof` = 16 928 B × 96 tiles. Its own comment says "96 KiB total" — **stale by 17×** (8 arrays grew to 33). Not a bandwidth item: ~1.3–3.8 KB touched per tile. |
| `FListEntry` | — | — | 24 B, not the 16 B its comment claims (bbox fields were added) — 2.67 per line, not 4. |

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
  **CEILING MEASURED 2026-08-06 and it confirms "low priority" with numbers**
  (docs/ENVDYN_DISPLACEMENT_PLAN.md §A4). Face-count ladder at t=5780 under
  `--greets_displace`: 6 522 faces (uniform L1) → 87 256 (edge carve) moves RNDR
  46.04 → 53.46 = **92 ns/face threaded ≈ 0.60 µs/face core**, i.e. **3.3–4.7×
  cheaper than B2's 2–2.8 µs serial** — `--tile_bbox_cull` took most of it.
  Halving the face count is worth −6.13 ms with the two new companions OFF, but
  with them ON the 87 k edge carve and the 43 k dome path measure **55.86 vs
  55.64, 0.22 ms apart**: the clone half of that work is already gone. So a
  per-chunk screen-space-error LOD is worth **≈0.2–3 ms** against a bake-time-only
  ladder (the bake is 2–6 s; no per-frame re-tessellation), 25–40 MB resident,
  and DMM's per-edge min-level rule to keep boundaries watertight when neighbours
  choose levels independently (today's border pins only close cracks for a FIXED
  level assignment). NOT built, deliberately; reproducible in three bench runs.

## Geometry front-end (XFRM) — measured 2026-08-05, docs/SOA_VERTEX_REFACTOR.md

> **RE-OPENED 2026-08-06 (b) with a CONTROLLED experiment — the mechanism is now
> quantified, and two items below are REPRICED.** `-DFDS_VERTEX_PAD_BYTES=N`
> adds dead tail padding to `Vertex`: not one instruction in any loop changes,
> only `sizeof`. greets t=5780 `--greets_displace`, 253 280 verts, per-frame min
> over 24, `pad=0` run first and last (drift 0.005 ms):
> **sizeof 140 → VERT 1.118/1.123 ms; 204 → 1.203 (+7.6 %); 268 → 2.315
> (+107 %).** Per-vertex time is a steep, super-linear function of the struct
> stride, with a cliff past ~256 B (stride prefetcher gives up).
> Bandwidth: ~284 B of streamed traffic per vertex at 4.41 ns = **64 GB/s on ONE
> core**, and this doc's own 958 k-vert numbers land at 64.3 GB/s (VERT) and
> 62.3 GB/s (the zero-arithmetic SoA sweep). Three unrelated loops at 62–64 GB/s
> is a single-core streaming ceiling — the one explanation for every wash on
> record (Vec8f, reciprocal estimate, single-precision divide, dropping 2 of 3
> mat-vecs, and the new field reorder).
> **Repricing:**
> - **Phase 5 / the interleaved 64-byte output array is CONFIRMED and worth MORE
>   than the −26 % below** — traffic ~284 → ~132 B/vert, i.e. on the order of
>   **2× VERT** against the measured slope. Blocker unchanged: `Vertex` must
>   split into mesh storage vs the clipper's transient type (every `RasterFunc`
>   takes `Vertex**`).
> - **"Read the per-face `Flags`/`PX`/`PY` from the SoA arrays" is REFUTED.** A
>   field reorder that puts PX/PY/Flags/TPos_AOS.z in 24 contiguous bytes cuts
>   the predicted lines per random 3-vertex deref 2.88 → 1.56 (−46 %) and moved
>   FACE by **0.6 %** (0.583/0.604 → 0.588/0.606). The A/B/C derefs are already
>   cache-resident. The 73 % is the branch chain + the per-face `F->Txtr->Flags`
>   chase + the Face stream, not the vertex reads. No `A/B/C_idx` invariant
>   needed, and no win there to collect.
> - **DONE 2026-08-06 — `--xfrm_par`, DEFAULT ON.** The main-view
>   `Transform_Objects` now runs on the pool: the object list is cut into
>   contiguous mesh-index BLOCKS (2 per worker) that are work-stolen off a shared
>   cursor, each appending into an FList segment **reserved in mesh order** (the
>   prefix sum of per-mesh `FIndex`), then compacted in block order — so
>   execution order is free while output order is pinned, and the result is
>   bit-identical to serial whatever order the workers finish in. Measured,
>   1920x1080, per-frame min over 24, min-of-arm over 3 interleaved reps, load
>   14-23: **displaced t=5780 1.546 -> 0.449 ms (-1.10, 3.4x); shipping t=5743
>   0.423 -> 0.261 (-0.16, 1.6x)**. Machinery overhead measured with 1 block
>   (inline, no dispatch): 10-35 us. Gates: render_gate 3/3 in both arms, city
>   `37e62845` and fountain `51fff7cd` PIN EXACT, chase 5-pose + cinematic and
>   greets t=1588 / t=5780 off==on, and **24 sequential runs of the greets pin
>   recipe with the parallel path on = 1 hash, 0 flips**.
>   **It also corrected the premise it was proposed on.** "The chip's aggregate
>   bandwidth is several times 64 GB/s" is FALSE for this access pattern: the
>   displaced arm streams 53.7 MB in 0.411 ms = **~131 GB/s, ~2x the single-core
>   figure**, and finer blocks do not move it — so that arm is now bandwidth-
>   bound at the SOCKET. The shipping arm is bound by something else entirely:
>   one mirror clone is 55 % of its main-view verts AND 55 % of its faces, so it
>   sits at the per-mesh LPT bound (predicted 0.221, measured 0.246). Full
>   working in docs/SOA_VERTEX_REFACTOR.md 2026-08-06 (c).
>   **Consequence for what is left:** more threads cannot help the displaced arm
>   (bytes/vertex can — Phase 5 / the interleaved 64-byte output array is now the
>   ONLY lever on it), and the shipping arm's lever is shrinking that mirror
>   clone (docs/VISIBILITY_PLAN.md 8e), which would help the serial path too.
> - **LANDED, byte-null, neutral:** the transform's `UZ`/`VZ` stores were dead
>   (the clipper overwrites them at entry) — 10 sites deleted in `fdc7a07`.

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
  the per-face dot) — ~~still unmeasured~~ **REFUTED BY MECHANISM 2026-08-06, do
  not build.** A normal cone can only remove the backface term
  `AP·F->N < F->NormProd`. Two facts kill it:
  1. That term is the CHEAP half of the per-face cull. The 73 % figure above is
     `--xfrm_ablate=8`, which runs `VisibilityFlagsAll()` **and** the dot; the
     expense is the three random derefs into 140-byte `Vertex` structs for
     `Flags`, while `F->N`/`F->NormProd` are Face-local and sequential. A cone
     cannot answer the frustum test, so it leaves the expensive part in place.
  2. It cannot help the SHADOW passes **at all** — `shadowNoBackface`
     (`Transform.cpp`, `g_inShadowPass && !shadow_backface_cull()`) sits before
     the dot in the `||` chain, so the dot never runs there. That is 60–80 % of
     the front-end ms.
  Its whole ceiling is the backface dot in MAIN + OFFSCREEN, inside a 0.18 ms
  main-view FACE bucket.

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
mirror RTT scope but **not** inside the env/SH probe bakes. Re-measured
2026-08-06 after the orphan-clone-vertex compaction (`964bf1d`,
VISIBILITY_PLAN §10): at t=5743 the four clones are **49 390 of 200 464
OFFSCREEN verts/frame (24.6 %)** in the shipping arm (was 68 540 of 219 614 =
31.2 % before the compaction) and **48 804 of 2 407 892 (2.0 %)** under
`--greets_displace`. Whether a probe should see a reflection at all is the
user's call; the machinery already exists — `g_envBakeSkipMirrorClones` /
`EnvBake_IsMirrorCloneObj` at `Transform.cpp:1481` structurally excludes
clones from probe bakes, but it is gated on **`--env_bake_fix`, which defaults
0**, so today's shipping probes DO see the reflections. Turning that on is the
switch; the saving is the 49 390 verts/frame above.

## Mirror clone — what is left after the orphan compaction (2026-08-06)

Context: `docs/VISIBILITY_PLAN.md` §10. `964bf1d` clones only the vertices a
surviving clone face references: displaced-arm MAIN 545 339 → 299 449
verts/frame (−45.1 %), shipping-arm mirror-panel poses 54 272 → 28 782
(−47.0 %, XFRM 0.304 → 0.189 ms), byte-identical at 21 gates in both arms.
Ranked leftovers:

- **~~`--mirror_clone_tight_bsphere` → default ON~~ — PARKED, MEASURED ZERO
  WIN.** Landed default-OFF in `964bf1d`. It is the *correct* sphere (a clone
  cannot draw a vertex it does not carry) and it measured byte-identical at all
  21 gates in both arms — but byte-identical **because it culls nothing**:
  main-view transformed verts are the same with it ON and OFF at all six
  (arm × pose) pairs measured. Even the correct tight sphere over a whole
  compacted clone is 0.0 % frustum-cullable at every pose. Keep the flag for
  whoever revisits a split; do not flip it.
- **Bound the OPAQUE clone raster by the mirror window.** TODO, and the
  remaining half of the user's original question. The TRANSPARENT path already
  does exactly this (`RENDER.CPP` ~936–990: a clone batch's bound is its
  mirror's stamped `gb.mirrorId` window, not the clone geometry's projection);
  the opaque path rasterises clone faces over their full projection and
  rejects them per pixel. RNDR was 7.19 of the clone's 11.40 ms in the
  pre-`1a91ed5` displaced arm. Needs no clone split. NOT measured post-`964bf1d`.
- **~~Spatial split of the clone (VISIBILITY_PLAN §8e)~~ — DE-SCOPED, MEASURED
  NOT WORTH IT.** Post-compaction the clone is ~27 k verts at the wall pose and
  **1 926** at the mirror-panel poses, against a 0.19–0.44 ms main-view pass, so
  §8b's "~2 ms" is void. Re-measured ceiling: at the panel poses a cell = 8
  split culls 98–100 % **of 1 926 verts** (~0.01 ms); at the wall pose the best
  granularity is per-source-mesh at 10 260 verts (~0.05 ms) and the spatial
  cells §8e specced are the *worse* of the two there (25.7 %). The margin cells
  used to hold was the orphan block, twice removed (`799c808`, `964bf1d`). Do
  not re-propose without a new measurement.

## Three architecture candidates, decided on measurement (2026-08-08)

Measured with `--deferred_prof` (`docs/PERF_STATE.md` §0). Read that section for the
tables; this is the disposition.

- **(C) make the filtered-albedo plane (`--texture_filter>0`) the default — KILLED.**
  The premise was that the G-buffer stores a texel ADDRESS, so the kernel pays a
  dependent random gather per pixel that a linear filtered plane would remove.
  Measured at 5 poses × 3 interleaved reps: the kernel is **not faster at any pose**
  (`lighting-w1` 27.32→27.29→27.53 at greets 5743; same flat result at greets 2000 /
  4200, city, fountain), while the raster pass pays a consistent **+1.2–1.4 ms** for
  the extra plane. Frame cost +0.9…+4.0 ms. Proven directly by `--prof_no_tex`:
  deleting the albedo gather outright changes the kernel by +0.9 % — there is no
  gather cost to recover, because the normal / metal / roughness / AO / horizon maps
  are still fetched at the same `Mipmap[mip][swizzledUV]` address. `--texture_filter`
  stays a quality flag. **Do not re-propose as a perf lever without new evidence
  against the `--prof_no_tex` row.**
- **(B) clustered / finer light assignment — NOT SUPPORTED as stated, but the light
  loop IS the elephant.** The omni loop is **20.9 ms = 45 % of the greets frame**
  (`--prof_no_lights`), the single largest slice. But finer binning attacks light
  COUNT, and count is not what sets the cost: the per-tile census (12×8 grid) gives
  greets **6.9 lights/tile → 20.9 ms** and city **26.1 lights/tile → 3.4 ms**. 3.8×
  more lights for 1/6 the cost. The difference is per-light WORK (greets' pixels are
  normal-mapped → scalar kernel path with cube-shadow taps; city's take the 8-wide vec
  path). Corroborating: `--cone_fine_tiles` on city — the same "finer tiles" idea on
  the pass that owns 44 % of that frame — measured **no gain** (30.87 vs 30.97 ms,
  3 reps each). Attack per-light cost, not lights-per-tile.
- **(A) material binning inside the tile — the honest ceiling is ~8.6 ms.** Binning a
  tile's pixels by matID would hoist the `matID → Material*` resolve and the
  has-normal-map / has-AO / has-roughness branches out of the inner loop. That work is
  the NON-light part of the shading wave, measured at **8.61 ms** (`--prof_no_lights`,
  greets 5743) out of a 46.4 ms frame — and binning would recover a fraction of it,
  not all of it. Not refuted, but it is an 18 %-of-frame target where (B)'s territory
  is 45 %, and the (C) result says the coherent-gather half of the argument is worth
  ~0.

**What the numbers actually point at, in order:** the per-light cube-shadow/PBR loop
on greets (20.6 ms, split 10.8 ms shadow sampling / 9.8 ms per-light shading math),
the volumetric cone pass on city (30.7 ms = 44 %, and not a tile-balance problem), and
`TBR_Render` on fountain (14.8 ms = 73 %). None of the three candidates on the table
addresses any of those three directly.

**The one measurement that would settle (B), and does not exist:** a per-PIXEL count
of lights that survive the range/cone test inside the kernel loop. The census counts
per TILE. If most of a tile's 6.9 lights are rejected per pixel, finer binning removes
the rejects and (B) is worth its 20.6 ms target; if most survive, finer binning
removes nothing and the lever is per-light cost (cheaper shadow tap, fewer shadowed
omnis). Cheap to add next to `FDS_TILE_LIGHT_PROF` in `DeferredSurfaceKernel.cpp`.

Also newly visible and previously unattributed (all `docs/PERF_STATE.md` §0):
- **city `cones-call` 30.7 ms = 44 % of the city frame** — the largest single phase
  in any scene measured, and nobody was looking at it. `--cone_fine_tiles` does not
  help (measured). Untouched question: what the cone pass actually costs per spot.
- **fountain `TBR_Render` 14.8 ms = 73 %** — the fountain is a TBR-bound frame, not a
  lighting-bound one (its whole deferred lighting is 1.7 ms).
- **the bloom chain (DoF + bright pass + anamorphic + bloom + lens ghosts) is
  ~1.8 ms/frame on greets** and sat OUTSIDE every existing timer until now.
- **`mirror-grid` 0.68 ms/frame** — a full-res scalar scan of the mirrorMask plane in
  the lighting setup, every frame, on every scene that has a mirror mask.
- **the mirror clone costs ~3.4 ms of G-buffer raster** on greets t=5743 (5.69 ms with
  `--greets_mirror`, 2.30 without).

## Static shadow lightmap: the atlas is sized by FACE COUNT, not area (2026-08-09)

`StaticShadowLightmap::data` is `numFaces * lmRes² * numOmnis` **bytes**, and
`allocate()` fills it with 255 — so every byte is touched, resident and counted.
greets sets `shadow_lightmap_res = 128` (`GREETS.CPP` `GreetsApplyInitDefaults`),
which is calibrated for its authored wall quads and scales with the wrong
quantity the moment anything is tessellated.

**MEASURED** (greets `t=5743`, `/usr/bin/time -l` peak footprint, 64 GB box):

| arm | baked faces | atlas store | peak footprint | bake |
|---|--:|--:|--:|--:|
| flat | 33 396 | 5.61 GB | 6.93 GB | 1.08 s |
| `--greets_displace`, before | 115 346 | 19.36 GB | 22.97 GB | 6.2–11.7 s |
| `--greets_displace`, after | 115 346 | **0.14 GB** | **2.35 GB** | **0.09 s** |

- **DONE for the displaced arm** — `--shadow_lightmap_texel_density` (default 0 =
  OFF = byte-null), defaulted to 14.2 texels/world-unit by `--greets_displace` as
  its third perf companion. Per-mesh res = `clamp(ceil(sqrt(meanFaceArea) *
  density), 8, shadow_lightmap_res)`; capped, so it can only reduce. Look cost in
  the displaced arm: **byte-identical at t=1588 / 2845 / 4871 / 6097, and 3 px at
  1 LSB at t=5743** — the 19.2 GB it removes was buying nothing.
- **DONE for the FLAT (shipping) arm too — 2026-08-09, at the user's
  instruction.** `setDefault(shadow_lightmap_texel_density, 14.2)` moved out of
  the `--greets_displace` branch and into the main `GreetsApplyInitDefaults`
  block, so both arms get it. **347 of the 370 baked meshes** fall under the 128
  cap (mean face edge 1.303 world → res 19); the 23 that keep the cap are the
  big authored quads. MEASURED on the flat arm at `t=5743`:

  | | legacy (`…density=0`) | default 14.2 |
  |---|--:|--:|
  | atlas store | 5.61 GB | **0.09 GB** |
  | peak footprint (`/usr/bin/time -l`) | 7.44 GB | **1.50 GB** |
  | static bake (min-of-9 interleaved, load 11–17) | 1104 ms | **54 ms** |
  | greets-entry join wait (load 31) | 3497 ms | **221 ms** |
  | frame ms `t=5743` / `t=5780` (min-of-15 interleaved) | 49.39 / 51.84 | 49.47 / **50.08** |

  **LOOK: NULL, and measured as such** — byte-identical at all 16 poses of
  `docs/greets_review_poses.txt` and at the pin pose, so the greets pin
  `778fa6ac…` did **not** move (4/4); city `3cbe42b1…` and fountain `8db68ccb…`
  4/4 each; `render_gate` 3/3. Revert flag `--shadow_lightmap_texel_density=0`
  reproduces the pin 4/4.
- **NEW, and bigger than the above: the shipping greets arm BAKES the atlas AND
  NEVER READS IT.** `DeferredSurfaceKernel.cpp:1619`
  `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()`; greets defaults
  `shadow_dynamic` ON and `shadow_lm_dynamic` is compile-default 0, so every
  pixel takes the cube tap. MEASURED: `--no-shadow_lightmap` renders
  **byte-identical** frames at t=5743 and t=6097, and forcing the atlas live
  with `--shadow_lm_dynamic` is byte-identical to the shipping frame as well.
  So the remaining 0.09 GB + 54 ms bake is *still* pure waste on the default
  path — the whole `LightmapBake_Static` call could be skipped when
  `shadow_dynamic && !shadow_lm_dynamic`. Not done here: it is an FDS/RENDER
  change, and someone may want `--shadow_lm_dynamic` to become the default
  instead (which is the opposite fix, and a look call).
- Stale comment left behind, worth a one-line fix by whoever next touches the
  file: `FDS/RENDER/LightmapBake.cpp:330-336` still says greets turns the
  density on "only under `--greets_displace`… so the byte-pinned FLAT path never
  takes this branch at all". Both halves are now false.
- Same for city/fountain if they ever raise `shadow_lightmap_res` off its
  default of 16.
- Related: the atlas is sampled per pixel by the deferred kernel
  (`DeferredShadowSampling.h` `resolvePixelLightmap` → `sampleBilinear`), so its
  size is a per-frame cache/TLB question as well as a startup one. On a 64 GB box
  with the array resident the per-frame difference at t=5743 measured **within
  noise** (min-of-6: 55.07 ms before vs 54.82 after); the cost that IS certain is
  the startup bake and the 20 GB of resident pages every other process has to
  live around.

## How this list is maintained
Add an entry the moment an optimization is deferred (with: what, why deferred,
where in code, expected cost/benefit). Mark DONE with the commit + measured
result. SESSION_STATE "Queued next" points here; the memory
`optimization-backlog` points here too.
