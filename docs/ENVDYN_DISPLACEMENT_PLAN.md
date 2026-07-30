# Campaign plan: AABB culling foundation + dynamic env reflections + stone displacement

Three connected workstreams from the 2026-07-30 research pair (env-cube
static/dynamic split; height-map→geometry), shaped by the user's directives:

- **Editor-flagged probes**: dynamic env bake only where the user marks it —
  authored control, not heuristics. "This should be enough to trim down the
  work significantly, and will give me control on where and what to bake."
- **AABB now**: no visibility infra exists; introduce world AABBs and use
  them to gate the dynamic bake (camera-cull) — and they serve the
  displacement workstream too. Precomputed visibility masks: rejected as
  overkill (user).
- **Mech bsphere** for fast probe-relevance rejection.
- **City depth on disk**: optional cache extension — the bake recomputes at
  start anyway; the disk copy is a dev-iteration optimization, cheap to add.
- Plus **slice 0**: the momy split renames to `momy-1`/`momy-2` (user
  preference).

Research grounding (file:line anchors verified at HEAD eb36c1f by the two
read-only research passes; re-verify on contact):

| fact | anchor |
|---|---|
| Env bake: lazy once-per-material, never re-baked | `EnvBake.cpp:959-974` (`EnvReflection_FramePrep`) |
| Bake renders 6 padded faces (kEnvCubePad 1.25 → ~102.7°) full-deferred, skipVolumetric | `EnvBake.cpp:625,315-329`, `EnvCube.h:41,63` |
| Per-face bake Z is scratch, FREED — stores keep color mips only | `EnvBake.cpp:359-360,443-455` |
| Dynamic meshes excluded from every probe (`g_envBakeSkipDynamic`) | `EnvBake.cpp:287`, `Transform.cpp:634,800` |
| Shadow dynamic-rebake machinery to mirror | `Shadows.cpp:173-175,230-343`, `DeferredShadowSampling.h:196` |
| Shadow cone-cull constants assume 90° pyramid — must re-derive for padded faces | `Shadows.cpp:304-306` |
| City disk cache: color only, FNV key | `DEMO/CityPanoramaCache.h/.cpp:49-72` |
| POM: G-buffer-fill march, measured +1.7-2 ms threaded (naive-8) | `Mekalele.h:1018-1160,1649-1668`, `docs/HEIGHTMAP_POM_PLAN.md` |
| Subdivision machinery exists, crack bug documented | `DEMO/MeshOps.cpp:856-962`, `GREETS.CPP:1390-1399` |
| Tile walk has NO per-tile face pre-reject (scaling hazard) | `RenderInner.cpp:220-247`, 6×5 tiles `RENDER.CPP:437-438` |
| Greets height maps: 'rooms'/'floor', 1024², ParallaxScale 0.25 | `GREETS.CPP:1268-1315` |

## Slice 0 — momy → momy-1 / momy-2 rename (independent, small)

User preference. Naming is free post-split-bake (nothing keys on `#k`), but
three CODE sites reference the literal `"momy"`:
`GREETS.CPP:1398` (`SubdivideMaterialFaces` hook), `GREETS.CPP:1636`
(`kSingleShadowIdMats`), `MeshOps.cpp:121` (strcmp). Touch list:
1. `Piramid.lwo`: SURF rename `momy`→`momy-1`, `momy2`→`momy-2` (extend
   tools/lwopatch.py with a surface-rename helper if absent; SURF name is a
   length-prefixed padded string — byte-diff must be name-only).
2. `Runtime/SCENES/GREETS.MAT`: rekey the 11 momy/momy2 lines.
3. The 3 code sites → `"momy-1"` (decide: does the second mummy need
   membership in kSingleShadowIdMats too? Check why momy is there — if it's
   a per-surface shadow-ID dedup, momy-2 likely wants the same).
4. Regen + install GREETS.FLD; greets gates (stable-pixel 5v5 + the
   deterministic momy close-cam `FDS_GREETS_CAM=-12.1,3.2,-27,0,-0.06,-1`).
Backups per the established greets discipline before touching.

## Foundation F — world AABBs + camera cull (shared)

- `TriMesh` gains a world-space AABB: static meshes computed once at
  load/init (post first full transform); dynamic meshes recomputed per
  frame from the posed verts' cached ranges or conservatively inflated by
  the spline extent (the `isDynamicForBake` heuristic already partitions
  static/dynamic — reuse it).
- Camera-frustum vs AABB test (engine today has only the symmetric-frustum
  sphere cull + `g_offAxisFrustumCull`): standard 6-plane AABB reject,
  usable against the MAIN camera frustum and against a probe's padded face
  pyramid.
- Consumers: W-A gating (below); W-B's chunk culling stays bsphere-based
  for now (AABBs available if measurement says they pay).
- Non-goals: no BVH, no occlusion, no precomputed visibility masks.
- Validation: flag-off byte-null everywhere; a debug overlay
  (`--draw-aabbs`-style, gated) for eyeballing; unit sanity via the
  existing snapshot scenes (a cull that fires wrongly moves pins — the
  pins ARE the test).

## Workstream A — dynamic env reflection overlay

Goal: the mech (and future movers) appears LIVE in reflections the user
marks — today it is structurally absent from every probe.

- **A1. Authored probe flag.** Per-surface `envDynamic` bit via the proven
  RVSF path (lwopatch → lwsread → FLD payload → `Material::EnvDynamic`),
  exposed as an editor Material-panel checkbox routed like the other RVSF
  keys (editor_server pop_rev_ext_props). Probes are per-material, so
  "flag specific objects" lands naturally as flagging their reflective
  surface(s). Default: nothing flagged → workstream is inert.
- **A2. Store retention for flagged probes only.** `EnvPanoStore` gains
  per-face static Z + pristine static color master, allocated ONLY when a
  store's owning material has EnvDynamic (memory scoped by the user's
  flags; ~768 KB Z + color master per 256² probe).
- **A3. Overlay pass** (ordered like RenderSecondOrderMirrors — before the
  tick's main Transform; NOT inside FramePrep, whose contract forces a
  full re-Transform):
  For each flagged probe within the staleness budget:
  1. Owner-visibility gate: probe's owning surface mesh AABB vs camera
     frustum (Foundation F) — offscreen owner ⇒ skip entirely.
  2. Relevance: mech bsphere vs probe (distance/range + per-face padded
     pyramid cull, cone constants re-derived for ~102.7°).
  3. Per touched face: memcpy static color+Z → scratch surf, dynamic-only
     transform (invert the `g_envBakeSkipDynamic` filter, mirroring
     `g_inDynamicShadowBake`), raster+shade LDR, writeback, per-face mip
     refilter (2×2 box, levels 1-3 — REQUIRED: rough surfaces sample
     mips; without refilter the mech vanishes on rough metals).
  - Feedback: overlay shade samples other probes' STATIC stores (recursion
    impossible inside OffscreenViewScope) — mech absent from 2nd-bounce
    reflections, accepted.
- **A4. City depth cache (optional, decoupled).** Extend
  `city_envmap*.bin` with per-face depth behind a format-tag bump so city
  probes can be flagged later; regeneration at start remains the source of
  truth — the disk copy is purely dev-iteration speed. May land any time
  or be dropped; A1-A3 do not depend on it (city `imported` stores simply
  reject the EnvDynamic flag with a log until depth exists).
- Flags: `env_dynamic` (default 0 — byte-null off), `env_dynamic_budget`
  (probes/frame, default 2).
- **Bench gate before any default flips**: the ~1-2 ms/frame estimate is a
  hypothesis; measure with the profiler overlay at a greets pose with the
  mech adjacent to a flagged floor/stairs probe. Record measured numbers
  here.
- Validation: default-off = render_gate 3/3 + city/fountain pins
  byte-equal + greets stable-pixel; flag-on = visual A/B (mech visibly
  reflected, no seams at probe face edges — padding covers straddle),
  determinism of snapshot poses.

## Workstream B — stone displacement (greets walls/floor)

Goal: real silhouettes + true self-shadowing for the block-scale relief;
POM keeps the fine detail from a residual map.

- **B1. Subdivision hole fix — DONE (2026-07-30), and the weld premise was
  WRONG.** Measured (`FDS_SUBDIV_DIAG`): the momy lathe is index-closed —
  boundary-edges raw=0, every edge keyed by raw vertex index is shared by
  exactly 2 faces, seam included — so "midpoint sharing misses coincident
  verts" never applied (position-welding actually CREATES non-manifold
  edges; do not weld). The real holes were three stacked defects in
  `SubdivideMaterialFaces`, all fixed:
  1. **Unguarded Phong projection** at creases/poles (4 edges with endpoint
     normals >90° apart, 165 >45°, one |N|=0 pole vert) displaced midpoints
     wildly → fixed by normalizing endpoint normals + fading the projection
     by agreement (w=clamp(dot,0,1)²; linear midpoint at creases).
  2. **Fold-over on sliver faces** (sagitta > triangle width) → fixed by an
     iterative fold-relaxation pass (folded sub-face ⇒ its midpoints revert
     to the linear split; monotone, converges ≤3 passes).
  3. **Stale `F->NormProd`** — the rebuild recomputed sub-face N but kept
     the PARENT's plane constant; the backface cull (Transform.cpp:1554)
     evaluates (N, NormProd) as a plane equation → wrong culls near grazing
     = the visible notches. Fixed: NormProd = -(N·A.Pos) per rebuilt face.
     This was the dominant visible mechanism.
  Proof: momy close-cam t=1588, `FDS_MOMY_SUBDIV=2` (hook now subdivides
  both mummies) renders closed + rounded, byte-deterministic 3/3
  (e08ab911…); flag-off byte-null (1d937118… = pre-change baseline);
  render_gate 3/3, city/fountain pins byte-equal, wasm links.
  Note for B3: 'rooms'/'floor' walls have hard 90° corners — the crease
  guard makes those edges subdivide LINEARLY (dot=0 → w=0), so the bake
  will not round wall corners; verify with FDS_SUBDIV_DIAG on contact.
- **B2. Spike (measure before building).** Enable subdiv on
  'rooms'/'floor' (post slice-0 names unchanged — these are wall/floor
  materials, not momy), read `[SUBDIV]` exact counts, `--bench`/profiler
  at L=1/2/3 at the POM-plan wall pose (greets t=5780). Decision gate:
  pick L from measured serial-transform + tile-walk cost, not estimates
  (prior estimate: L=2 ≈ +15-30k tris ≈ few ms; L=3 needs B5).
- **B3. Displacement bake at init** (at the FDS_MOMY_SUBDIV hook point:
  after material/HeightMap load, BEFORE `MakeFacesIndependentByAngle` and
  the chunk split so chunk bounds wrap displaced verts): weld → subdiv L →
  per-vertex sample HeightMap at low mip (4-5) at the per-FACE UV → push
  along vertex normal by `displace_amp*(h-0.5)` (world-unit flag; the
  UV-space ParallaxScale is not reusable). Normals re-derived by the
  existing MakeFacesIndependentByAngle; tangents downstream as today.
- **B4. Residual height map**: full-res height minus upsampled displaced
  component (via MakeHeight8/Scene_MakeTiledTexture), installed as the
  POM input so relief isn't double-counted; re-run MakeConeMap when cone
  POM is enabled.
- **B5 (conditional on B2, independent perf win)**: per-face screen-bbox
  pre-reject in the tile walk (`RenderInner.cpp:220-247`) — 4 compares
  against the tile rect before clipper entry. Do it if L=3 is wanted OR
  if the spike shows the walk cost is already material at L=2.
- Flags: `greets_displace` (default 0 until validated + user-approved
  look), `displace_amp`.
- Validation: flag-off byte-null (greets stable-pixel + other pins
  untouched); flag-on visual A/B at silhouette-revealing poses (doorway
  jambs, wall-floor junctions, grazing wall view); snapshot determinism;
  static shadow lightmap memory delta recorded (scales with face count).

## Sequencing / ownership

```
slice 0 (rename)          — independent, first (touches greets content while quiet)
F (AABB)                  — before A3; small, lands with A-stream
A1 → A2 → A3 (→ A4 anytime/optional)
B1 → B2(spike gate) → B3 → B4 (→ B5 per spike)
```
A-stream and B-stream are parallelizable after slice 0 (disjoint files:
EnvBake/Transform/editor vs MeshOps/GREETS init/Mekalele residual), with
Foundation F owned by the A-stream. Every slice: render_gate 3/3, pin
table respected, greets via the stable-pixel 5v5 method (raw hash-majority
is dead — screen-glow wall-clock phase, see SIDECAR_MIGRATION_PLAN §6),
wasm link check when engine sources change, headless dummy-driver runs
only, flags default-off until the user approves looks.

## Status

- [x] Slice 0 rename (f4f2e38 + 0808bd1)
- [ ] F AABB foundation
- [ ] A1 editor-flagged probes  · [ ] A2 store retention · [ ] A3 overlay
      · [ ] A4 city depth cache (optional)
- [x] B1 subdivision holes (weld premise disproven; Phong guard +
      fold-relax + NormProd recompute) · [ ] B2 spike (numbers → here) ·
      [ ] B3 displace · [ ] B4 residual · [ ] B5 tile pre-reject
      (conditional)
