# Rendering roadmap

Forward-looking plan for the rendering pipeline. Each section has:
**Goal** (what we want), **Current state** (what's already there),
**Options** (with cost / quality tradeoffs), **Recommended path**,
**Effort estimate** (S=days, M=week, L=multi-week), and **Open
questions** where they exist.

The "Pick-first" section at the bottom ranks everything by ROI and
risk so we can attack one item at a time.

---

## 1. City reflections

**Goal**: convincing reflections in the office-tower glass windows
that respond to the city environment. Per-window viewport renders are
off-limits (N × scene cost is orders of magnitude over budget).

**Current state**: `bakeBuildingPanorama` already bakes a cube-map-ish
panorama per building once, used at runtime via reflective filler
routing through `TheOtherBarry<OVERWRITE, TEXTURETEXTURE>`. Memory
notes a `cv-pull instability` bug — `Reflective_Mapper_Setup` divides
by `step = pullDir·N`, which passes through small values and makes
reflections swing wildly with small camera moves. Repro is in the
`--snapshot=seaside distsweep` block.

**Options**:
- **A. Fix cv-pull stability** in the existing per-building cube
  approach. Pure bugfix — no new rendering tech. Probably the
  divisor clamp or a robust replacement formula.
- **B. Parallax-corrected cube** on top of (A). Offset the cube
  sample by the (window-pixel → cube-center) direction; sharply
  improves "wrong angle" issues at oblique view. ~5 fmuls/pixel
  extra in the reflective filler.
- **C. Screen-space reflections (SSR)** for dynamic content
  (vehicles reflecting off buildings). Ray-march the deferred depth
  buffer along the reflection vector; fall back to (A)/(B) when the
  ray exits the screen. Visible only for on-screen reflectors but
  cheap.

**Recommended path**: A → B → maybe C later. (A) on its own probably
removes 90% of the visible badness; (B) is icing.

**Effort**: A=S, B=S, C=M.

**Open questions**: cv-pull derivation isn't documented anywhere I
found. Need to derive the right divisor or accept a clamped version
with bias.

---

## 2. Volumetric fog + vehicle spotlights

**Goal**: visible "shafts" of light from car headlights / streetlight
spots through hazy city air. Greets benefit from atmospheric
volumetric glow around the orbit spots too.

**Current state**: deferred kernel has fog/depth-falloff
(`Add fog/depth-falloff to deferred lighting kernel` in
commit history). One exp per pixel. No per-light volumetric.

**Options**:
- **A. Per-spotlight cone-proxy mesh**: render a back-facing cone
  primitive (or capped billboard) per spotlight in the transparent
  pass, with additive blend + soft alpha falloff toward the cone
  axis. Depth-test gives "cone-cut-off-behind-wall" automatically.
  ~16 verts per cone, near-free.
- **B. Layered full-screen additive masks** for global haze
  (ground-fog layer brightens additively with each visible light).
  Cheap, looks fake but acceptable at the 1998 aesthetic.
- **C. Single-light ray-marched volumetric**: march N samples
  (16-32) along view ray from camera to surface, accumulate
  in-scatter. Expensive but doable for a few lights. Modern AAA does
  this with froxel grids (3D textures) — overkill here.

**Recommended path**: A for vehicle/spotlight cones. B optional for
ground haze. Skip C.

**Effort**: A=M (need transparent-cone primitive integrated with the
TBR/transparent path), B=S, C=L.

**Open questions**: where to inject the cone draws in the pipeline
— probably alongside the existing transparent fountain particle
path, since the depth-test + additive shape is identical.

---

## 3. Fountain — more lights

**Goal**: richer per-pixel lighting in the water column / base.

**Current state**: 4 strategic particle-path lights already (memory
`Add strategic particle-path lights to fountain scene`). The deferred
kernel handles them via per-tile light culling, so adding more
non-shadowed lights is cheap (linear-in-N per lit pixel, but tile
culling drops most lights per tile).

**Approach**: add 4-6 more **non-shadow** point lights — orange
inside the water column, blue-white at the base, maybe a few
moving with the particle splines. No new shadow maps (those would
multiply the per-frame shadow Transform_Objects cost).

**Effort**: S.

**Open questions**: are there visible light spots in fountain right
now that would benefit (i.e., are most pixels far from the existing
4 lights)? Need to eyeball or render a "lights-per-pixel" heatmap.

---

## 4. Profile-guided perf work

**Goal**: know where time actually goes before optimizing, so we
don't tune the wrong loop.

**Current state**: we have `[SHADOW-PROF]` and a frame-level
`FrameProfiler` with PROF_SORT/RNDR/etc., but no per-pass
millisecond breakdown of the deferred lighting kernel itself, no
per-tile cost histogram, no instructions-vs-cache-miss data.

**Approach**:
- Add `[DEFERRED-PROF]` per-tile timing — wall-clock per tile, plus
  per-tile counters: # lights evaluated, # nmap samples, # shadow
  samples, # vec vs scalar-path hits. Gated on a FeatureFlag.
- Run with Instruments (macOS) or `perf` (Linux) to collect IPC,
  cache-miss rate, branch-miss rate per region.
- Compare against expected memory bandwidth ceiling (e.g.,
  ~50GB/s on M-series). If deferred lighting hits >30% of that, it's
  memory-bound; if <10%, it's compute-bound.

**Effort**: S to instrument, plus a session to interpret numbers.

**Open questions**: nothing prescriptive until we have data. Likely
guesses (to be verified):
- Deferred kernel: memory-bound on G-buffer reads + shadow map taps
- Rasterizer: 50/50 compute (SIMD) / memory (texture cache)
- Shadow Transform_Objects: pure compute, well-parallelized
- Sort: negligible

---

## 5. Render-code improvements (small specific wins)

**Goal**: micro-optimizations identified during the bump
investigation that are low-risk to land.

**Wins**:
- **Vec-path shadow attenuation**: the `run_vec_diff_loop` and
  `run_vec_spec_loop` don't apply `shadowAtten`. Bumped surfaces use
  scalar (correct), but non-bumped meshes (vehicles, particles)
  potentially leak light/spec through shadows. Effort S.
- **MeshOps bake rsqrt**: bake uses `1.0f/std::sqrt(...)`; replace
  with `fast_rsqrt`. Bake runs once per scene so impact is
  modest. Effort S.
- **Shadow slope-bias div**: scalar path does
  `1.0f/(nDotL > 0.2 ? nDotL : 0.2)` per shadowed pixel — that's a
  real fdiv. Pre-clamp then `fast_rsqrt(x)²` or precompute reciprocal
  table. Effort S.
- **Half-vector renormalize**: kernel computes `(L+V)/|L+V|` then
  `dot(N,H)`. Could compute `dot(N, L+V) / |L+V|` to defer the
  normalize, or use unnormalized half-vector with Blinn-Phong
  approximation. Saves one rsqrt per lit pixel. Effort S.
- **Per-tile material precompute**: each lit pixel reads
  `Mat->Diffuse`, `Mat->Specular`, `Mat->Glossiness`, `Mat->Luminosity`.
  Cache them as floats once per tile. Effort S.
- **mat32 unpack SoA**: currently `(mat32 >> 28) & 0xF` per pixel;
  pre-decode miplevel/matID/swizzledUV into separate planes during
  Mekalele write. Higher cost for non-deferred reader but cleaner.
  Effort M.

**Recommended path**: vec-path shadow attenuation first (it's a known
correctness gap + small perf hit). Other items hinge on whether the
profile shows the deferred kernel as the bottleneck.

---

## 6. Baked static lighting upgrade

**Goal**: greets' 7 fixed FLD omnis (ent1/ent2/ctr1/ctr2/crd1/hal1/hal2)
currently bake into per-vertex `T->SL[]` without visibility — they
pass through walls. We want them to **cast (pre-baked) shadows**
without paying any runtime cost.

**Current state**:
- `StaticLighting(Sc)` (Lighting.cpp:36) bakes Omni_Stationary
  lights into per-vertex `T->SL[]` once per scene.
- Per-vertex frequency is mesh-resolution-bound. Smooth walls have
  few vertices → very low-frequency shadow result. Detail is lost.
- No visibility test currently — every static omni illuminates every
  vertex in its range, regardless of occlusion.

**Options**:
- **A. Add ray-traced visibility** to the existing StaticLighting
  bake loop. Per (vertex, light) pair, shoot a ray to the light;
  shadow if blocked. Visibility cached into `T->SL[]` as
  pre-attenuated value. Cost is at scene load (seconds).
  No runtime change. Effort: M (need a ray-traced occlusion query
  against the scene's TriMeshes).
- **B. Per-texel lightmaps** for static lights. Major upgrade:
  unique lightmap UV per surface (auto-atlas required), 1-4 bytes
  per texel, sampled at runtime. Soft shadows + indirect bounces
  possible. Effort: L (lightmap UV generation is the hard part).
- **C. Hybrid**: keep A as the cheap stepping stone. Tag certain
  surfaces as "lightmap-quality" and only those get B. Defers the
  full atlas problem.

**Tradeoffs**:
- **A pro**: keeps the existing T->SL data path. Per-frame cost is
  unchanged. Greets gets crisp(er) shadowed contributions from
  ent1/ctr1/etc.
- **A con**: per-vertex frequency means shadows only resolve at
  mesh-vertex granularity. Walls with 4 verts have 4-vertex shadow
  detail. Subdividing the mesh helps but blows up vertex count.
- **B pro**: per-pixel detail. Soft shadows. Indirect bounces.
- **B con**: huge engineering investment for the UV atlas. Memory
  per surface. Doesn't help moving lights (most of greets is moving).
- For City: B is a much bigger win — most City lights are static
  (windows, streetlights, sky ambient). Walls are large and flat,
  ideal lightmap targets.

**Recommended path**: A for greets soon, B for city much later
(after profile-guided perf work + HDR investment).

**Effort**: A=M, B=L.

**Open questions**:
- Auto-atlas algorithm choice (xatlas? simple boxpack?).
- Lightmap resolution policy — fixed (e.g., 1 texel per world unit)
  or per-surface adaptive?
- Indirect bounces — even one bounce massively changes the look.
  Path-traced or radiosity?

---

## 7. HDR + tone mapping + bloom

**Goal**: real specular sells the "shiny floor / hot reflection"
look. Currently `outR/G/B` clamps to 255 — any pixel that should be
"hotter than white" gets clipped to white, losing the dynamic-range
character that makes reflections feel real.

**Approach**:
- Render G-buffer + lighting output into `float` or
  `uint16_t` framebuffer instead of `dword`.
- Tonemap at the final composite step (Reinhard or
  exposure-then-clamp).
- Bloom: extract bright pixels (luminance > 1.0) into a small
  half-res buffer, gaussian-blur, add back. Cheap downsampled passes.

**Tradeoffs**:
- **Pro**: dramatic visual upgrade. Specular highlights become
  actual highlights instead of clamped white blobs.
- **Pro**: physically-grounded — lets us crank Specular without
  losing diffuse fidelity.
- **Con**: every output write doubles or quadruples in size — memory
  bandwidth doubles. Composite pass adds ~1ms.
- **Con**: textures authored for LDR may need re-authoring (user
  flagged). Or we apply an inverse-tonemap to LDR textures at load.
- **Con**: SDL2 surface format change (or composite-to-display
  conversion at flip time).

**Effort**: L (touches G-buffer layout, lighting kernel writes,
display backend, and possibly texture loader).

**Open questions**:
- Float G-buffer (full 4×float per pixel = 16 bytes/px = ~32 MB at
  1920×1080) might be too memory-bandwidth heavy.
- uint16_t with fixed-point gives 4×2=8 bytes/px = ~16MB; less
  range but cheaper. May be the right pragmatic balance.

---

## 8. TAA / temporal accumulation

**Goal**: smooth out per-frame variance in shadow rasterization
(memory documents 16-byte byte-diff between frames in the modal
shadow output). Also useful as a cheap supersampling path.

**Approach**: keep a 1-frame history buffer of the lit framebuffer.
Reproject previous frame's pixels into current frame's view (using
the camera matrix delta). Blend: `current = a*new + (1-a)*reprojected`
with `a≈0.1`. Reject reprojections that fail a depth/normal
similarity test (disocclusion).

**Tradeoffs**:
- **Pro**: free anti-aliasing on edges, smooths shadow noise.
- **Pro**: prepares the ground for stochastic effects (jittered AO,
  random shadow PCF samples) without flicker.
- **Con**: motion blur artifacts at fast camera motion (ghosting).
- **Con**: requires camera matrix history (small).
- **Con**: doesn't compose well with snapshot harness (every snapshot
  starts cold).

**Effort**: M.

**Open questions**: do we want it as a feature flag, or always on?

---

## 9. Profiling + visual debugging infrastructure

**Goal**: make perf and visual debugging fast iterative loops.

**Approach**:
- **Frame timeline overlay**: each PROF phase as a horizontal bar
  with ms width. Currently per-phase ms is printed; visualizing in
  the frame would let us see proportions at a glance.
- **Tile-cost heatmap**: false-color overlay showing per-tile ms or
  per-tile light count. Already-prototyped pattern (memory mentions
  `viz_normal`, `viz_tangent`, etc.).
- **Light volume viz**: draw wireframe spheres for each omni's
  range. Useful for `tl.range2` debugging.
- **G-buffer channel inspectors**: `FDS_VIZ_*` we already added.
  Add `viz_mat32_id`, `viz_miplevel`, `viz_shadow_atten`.

**Effort**: each viz S, all together M.

---

## 9b. SDL V_Flip overhead

**Goal**: greets bench at ~33 ms/iter has ~2.5 ms of V_Flip
overhead per frame. The wasm port previously got V_Flip to ~0.1 ms
through SDL configuration tweaks (lockTexture vs UpdateTexture
pattern, software-renderer hint, etc.). Same optimizations may apply
natively.

**Current state**: V_Flip in DEMO/SDL2.cpp does texture
update + RenderCopy + Present. The resolution-overlay we
just hit (top-right `WxH` string) is drawn inside V_Flip.

**Approach**: walk the wasm-port's V_Flip path (likely on master or
a wasm branch) for the techniques used. Candidates:
- Replace `SDL_UpdateTexture` with `SDL_LockTexture` + direct write.
- Drop intermediate texture if rendering to an offscreen render
  target.
- Force `SDL_RENDERER_SOFTWARE` if the GPU upload is the bottleneck.

**Effort**: S. Mostly investigation — the wasm port's commit history
should show the techniques.

**Open questions**: which branch/commit captured the wasm
V_Flip work? Need to find that first.

---

## 9c. Initialize_City / Initialize_Greets coupling

**Goal**: untangle the hidden dependency where `Initialize_Greets()`
silently relies on `Initialize_City()` having run first (textures,
skycube, material library).

**Current state**: snapshot harness and bench mode both call both.
A `// must call Initialize_City first` comment at Snapshot.cpp:213
acknowledges the issue. Sub-agent investigation ongoing.

**Approach**: identify the exact global state, extract a
shared `Engine_Init()` function, decouple. See
agent report for the dependency table.

**Effort**: M.

---

## 10. Tracked bugs / cleanups

Not feature work; flagged so they don't get lost.

- **Shadow indicator text width**: `kCharPx=10` approximation
  doesn't match `OutTextXY`'s variable-width glyphs. Compute true
  pixel width via `Active_Font->Len[Ch]` walk. S.
- **Vec-path shadowAtten** (also in §5): bumped surfaces shadow
  correctly; non-bumped (vehicles, particles) don't.
- **cv-pull instability** (also in §1).
- **Fountain snapshot harness hang**: task #14, pending.
- **Fountain transparent triangle clipping bug**: task #15, pending.

---

## Pick-first ordering

Ranked by (visual impact × ROI / risk):

1. ~~**Profile first (§4)** — small effort, unblocks the perf debate.~~
   DONE (2026-05-17): greets@t=2500 is 33 ms/iter, top costs are
   Render_DeferredLighting tile lambda (52%), psynch_cvwait (24%),
   meka::TileRasterizer (10%), FrustumClipper::Render (5.5%).
   FeatureFlags::get was 1.5% per-pixel — hoisted out, gone from
   top symbols.
1b. **V_Flip overhead investigation (§9b)** — ~2.5 ms/frame on
   native, the wasm port hit ~0.1 ms. Likely SDL UpdateTexture vs
   LockTexture. Almost-free perf if it transfers.
2. **City cv-pull stability fix (§1A)** — bug repro exists,
   moderate visual impact, removes a known annoyance.
3. **Vec-path shadow attenuation (§5 / §10)** — known correctness
   gap, small commit, makes vehicle/particle shadows correct.
4. **Per-spotlight cone primitive for fog (§2A)** — high visual
   impact, fits the existing transparent path.
5. **Add visibility to StaticLighting bake (§6A)** — closes the
   greets static-omni fidelity gap (user-flagged).
6. **Shadow indicator true-width fix (§10)** — small, isolated.
7. **City reflections: parallax-corrected cube (§1B)** — on top of (1)
   after stability is good.
8. **More fountain lights (§3)** — incremental, low risk.
9. **HDR + bloom (§7)** — large investment; do after profiler proves
   we have the bandwidth headroom.
10. **TAA (§8)** — depends on HDR for full benefit.
11. **Per-texel lightmaps (§6B)** — long-term, do after HDR.
