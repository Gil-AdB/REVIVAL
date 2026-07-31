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
- **B2. Spike — DONE (2026-07-30), MEASURED. Decision: L=2.**
  Knob: `--greets_stone_subdiv=N` (LINEAR midpoints — no PN rounding on
  flat walls; a `phong=false` mode added to SubdivideMaterialFaces).
  `[SUBDIV]` counts (Piramid mesh, base 5532 faces): 'rooms' 196 →
  784 (L1) → 3136 (L2) → 12544 (L3) faces; 'floor' 30 → 120 → 480 →
  1920. Total added: **+678 (L1) / +3390 (L2) / +14238 (L3)** — the
  prior "+15-30k @ L=2" estimate was 4-8× high.
  Cost, wall pose `--bench=scene@scene=greets,t=5780` 1080p (whole-frame
  mean; single-thread ±0.5 ms over 3 interleaved rounds; threaded = 3
  rounds, 40 iters, load ~10-17 bg):
  | L | ST frame | Δ | THR frame | Δ | ST p50 Δ: RNDR / BAKE / XFRM / SORT |
  |---|---|---|---|---|---|
  | 0 | 340.2 | — | 48.4 | — | — |
  | 1 | 343.7 | +3.5 | 49.2 | +0.9 | +1.4 / +1.4 / +0.07 / +0.004 |
  | 2 | 355.0 | +14.7 | 51.0 | +2.3 | +9.5 / +4.4 / +0.30 / +0.02 |
  | 3 | 385.4 | +45.2 | 60.1 | +11.5 | +28.0 / +14.3 / +1.27 / +0.11 |
  Pin pose (t=1588, SESSION_STATE cam; noisier): ST L2 +10.5 / L3 +38.5;
  threaded paired-Δ medians L2 ≈ +5.7 (0.2-7.8), L3 ≈ +9 (8.6-26). Here
  BAKE dominates (ST p50 Δ +4.6 L2 / +15.9 L3) — the per-frame shadow
  bake re-rasters the subdivided walls regardless of camera; RNDR delta
  is pose-dependent (walls off-center → small).
  RNDR delta ≈ 2-2.8 µs/face SERIAL (per-face fixed cost — tile
  walk/clipper entry — not pixel work; these sub-faces add no coverage).
  **Decision: L=2 ships as the displacement density (+2.3 ms threaded at
  the worst pose). L=3 (+11.5 ms) is out: it needs BOTH B5 (RNDR share)
  AND a shadow-bake face-cull (BAKE share — B5 does not touch it), so
  B5 alone cannot rescue it. B5 verdict: NOT required at L=2 (threaded
  RNDR share ≈ +1.4 ms); optional, revisit only if L=3 is wanted.**
- **B3. Displacement bake at init — DONE (2026-07-31).**
  `--greets_displace` (default 0, byte-null off) at the momy hook point:
  linear subdiv (L = greets_stone_subdiv, or the B2 default 2 when unset)
  → `DisplaceMaterialVertices` pushes interior verts along their smooth
  vertex normal by `greets_displace_amp*(h − mipMean)`, h bilinear-sampled
  from the material's 8-bit HeightMap at `greets_displace_mip` (default 4
  = 64²) at the per-FACE UVs, averaged over incident target faces.
  Design deltas from the sketch (all evidence-driven):
  - **No weld** (B1 disproved the premise). **Mean-centering** replaces
    (h−0.5): the floor map's mean is 0.34 — centering on 0.5 gave a DC
    sink fighting the pinned borders instead of relief.
  - **Border pinning**: any endpoint of an edge used by exactly one
    target face (patch border: wall-ceiling/wall-floor junctions, jambs
    — also catches subdivision midpoints on the border whose T-junction
    against the unsplit neighbour would crack) + any vert incident to a
    non-target face is pinned at zero displacement. Relief fades to the
    authored edges; junction verified continuous at t=5780.
  - **Face N + NormProd re-derived** for displaced faces (B1 lesson);
    vertex normals + tangents re-derived downstream by
    MakeFacesIndependentByAngle as today.
  - **Degenerate-map guard**: near-constant height mips skip the bake.
    FOUND: the shipping `greets_wall_h.png` is ALL-WHITE (5.5 KB
    placeholder from the stone3 swap; the real 358 KB map sits in the
    user's `_bak_greets_wall_*` dirs — his call to reinstate; it also
    means wall POM has been marching a constant field). Walls therefore
    displace only via `--material-import=rooms:DIR` overlay or once a
    real map ships; the floor displaces on shipping content.
  - `greets_displace_amp` default **0.3** (world units): chosen by A/B
    at t=5780 with the real wall map — 0.1 invisible, 0.3 reads as
    uneven stone courses without distortion, 0.6 strong. Floor relief
    stays subtle (low-contrast map, ±0.022 at 0.3).
  Numbers: shadow-cluster inflation 2183→2925 (no acne seen); SL
  (per-vertex static-light) delta at L=2 ≈ 2221 verts × 16 B ≈ 35.5 KB.
  Flag-on deterministic (momy close-cam 2/2 byte-identical); flag-off
  byte-null vs HEAD binary (19ba82cf…, re-baselined after the user's
  GREETS.MAT `momy-2|smoothAngle|30` edit moved greets content mid-work);
  render_gate 3/3, city/fountain pins byte-equal, wasm links. A/B PNGs
  under /tmp/displace/.
- **B4. Residual height map — DONE (2026-07-31).** `MakeResidualHeight`
  (MeshOps): res[m](x,y) = clamp(h[m] − bilinear(h[lowMip], uv) +
  mean_low) evaluated per mip (mips ≥ lowMip flatten toward the mean —
  geometry carries those scales; sub-block mips byte-copied), same 8-bit
  tiled+mip layout so the kernel's swizzled index works unchanged.
  Installed as `M->HeightMap` for 'rooms'/'floor' when `greets_displace`
  is on; `MakeConeMap` re-baked from the residual when cone POM is
  active. Degenerate sources return null (bake skipped them; original
  map stays — the all-white shipping wall map case). Verified at t=5780
  close-up: POM grooves persist on top of the displaced geometry, and
  the double-counted state (displacement + full-map POM, captured
  pre-residual) showed ragged doubled grooves that the residual removes.
  Displacement OFF leaves the original map untouched (wiring is inside
  the displace branch; flag-off byte-null re-proven).
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
- [x] B1 subdivision holes (weld premise disproven; Phong guard +
      fold-relax + NormProd recompute) · [x] B2 spike (numbers in B2
      above; decision L=2, B5 not required at L=2) ·
      [x] B3 displace (--greets_displace, default OFF, amp 0.3) ·
      [x] B4 residual · [~] B5 tile pre-reject (measured non-required at
      L=2; only for an L=3 ambition, alongside a shadow-bake cull).
      WORKSTREAM B COMPLETE except the conditional B5 (declined on the
      B2 numbers). Content item RESOLVED (2026-07-31): greets_wall_h.png regenerated from
      the stone3 normal map via tools/nmap2height.py (Frankot-Chellappa FFT
      integration, sign auto-calibrated bricks-high/mortar-low against the
      albedo, polarity matches the POM march's white=protruding convention).
      The old pre-stone3 358KB map did NOT match the stone3 brick pattern —
      restoring it would have put grooves in the wrong places.
- [x] **F AABB foundation** — TriMesh world AABB (static once / dynamic per-
      frame from posed local-AABB corners) + world-space `Frustum` (main
      camera OR padded probe face) + AABB/sphere reject + `--draw_aabbs`
      overlay. `FDS/RENDER/WorldAabb.{h,cpp}`. Flag-off byte-null (render_gate
      3/3 + city/fountain pins). (commit 0d8f217)
- [x] **A1 editor-flagged probes** — `envDynamic` RVSF bit **0x400** (ascending
      order; 0x800+ reserved for slice 1.6) → `Material::EnvDynamic`, editor
      'dynamic env' checkbox. lwsread regen inert (city/crash/pbrtest golden
      md5s); authored path proven (FLD +3B, Surf_RevExt set). (13a700b)
    · [x] **A2 store retention** — `EnvPanoStore` keeps a pristine static colour
      master + the per-face depth the bake used to free, allocated ONLY for
      `EnvDynamic` cube probes. **~2.25 MB / 256² probe** (768 KB Z + 1.5 MB
      colour). (c7dcfc6)
    · [x] **A3 overlay** — `--env_dynamic` (default 0) + `--env_dynamic_budget`
      (2). Per flagged probe: camera-frustum owner gate → mover-bsphere /
      padded-face-pyramid touch cull → dynamic-only raster+shade per touched
      face → composite over static (depth-occluded) → per-face mip refilter.
      **BENCH (measured, not estimated):** greets pin pose, throwaway `floor`
      probe flagged, mech adjacent → **1 probe / 1 touched face / 7 movers /
      256² face = ~1.3–1.6 ms** (`FDS_ENVDYN_PROF`). A/B: env_dynamic ON vs
      OFF moves 2042 stable floor-reflection px (the live mech). Flag-off
      byte-null (render_gate 3/3 + city/fountain + greets momy2-cam 3/3
      `38518154`). (33ba879)
    · [ ] **A4 city depth cache (optional) — SKIPPED.** A1–A3 do not depend on
      it (city `imported` stores simply carry no retained depth, so a flagged
      city surface gets no overlay until this lands). Deferred: extend
      `city_envmap*.bin` with per-face depth behind a format-tag bump so city
      probes can be flagged later.
    · [x] **A3 fix (2026-08-01, 837c584): owner gate on the owning FACES' AABB.**
      User bug "stairs/screen dynamic bake doesn't work". Root cause (measured
      via the new `FDS_ENVDYN_PROF` per-probe WHY trace — NO-STORE /
      NOT-RETAINED / OWNER-OFFSCREEN / NO-MOVER-RELEVANCE / BUDGET-DEFERRED /
      OK + a composited-mech-texel census): the A3 step-1 owner-visibility gate
      used `WorldAabb_ForMaterial` = the union of whole-MESH AABBs, and greets
      merges every flagged surface into the chunked Piramid mesh — every
      probe's "owner box" was the whole scene (124×18×203), the gate never
      culled, and with 5 flagged retained stores vs budget 2 the round-robin
      spent slots on OFF-SCREEN probes (~6.7–9.1 ms/frame of invisible work;
      a framed probe refreshed only intermittently). Fixed: the gate now tests
      the probe's owning FACES' world AABB (`materialFaceAabb`, cached in the
      store). Per-face overlay cost unchanged (~1.2–1.9 ms per 256² face —
      the A3 bench number; ~2.2–2.4 ms per authored 512² face); total cost
      strictly drops (only visible probes refresh).
      **Timing caveat (user-corrected):** the mech reaches the stairs only at
      END of scene — its path sits in the main rooms (z −10..−53) until
      t≈6000, then walks to the staircase and parks ABOVE it at
      (44.8, 4.6, −62.4) from t≈7500 on. Early/mid-scene the stairs/screen
      probes correctly show NO mech (census: 100 % of rasterised mech texels
      z-culled by the walls between — physically right, not a bug). Verified
      at t=7500, stairs framed: stairs composites ~1,673–1,686 mech texels
      (3 faces), stairs::mirUV 862, screen emiter ~12,500 (6 faces); on-screen
      A/B (env_dynamic ON vs OFF, bake-noise-stable pixels only) moves 18,335
      px concentrated on the staircase reflection under the mech (noise floor
      91,883 raw / 0 on the stable set). Repro trap: `--snapshot` runs ONE
      tick and probes bake inside that tick's render, so the overlay never
      fires there — use `--bench=scene@scene=greets,t=…` or a two-timestamp
      snapshot (`--snapshot=greets@t=7400,7500`; the 2nd frame overlays).
      Also landed with the fix: `envDynamic` on a surface with no
      envRefl/Reflection/metallic now IMPLIES probe qualification under
      `--env_dynamic` (EnvReflMode==1 force-bake semantics; explicit
      envRefl=−1 stays honoured) + a one-line stderr warning names any
      surface whose flag would otherwise be a silent no-op. EDITOR follow-up
      (open, other agent): the 'dynamic env' checkbox should imply/require
      envRefl on save and flag the envRefl=−1 conflict inline.

### Foundation-F padded-face-pyramid cull derivation (A3 step 2)

`Frustum_FromProbeFace` builds the **tight 4-plane** pyramid for a padded cube
face, not a circumscribed cone: the padded face half-FOV has `tan θ =
kEnvCubePad = 1.25`, so the four side planes have view-frame inward normals
`(±1,0,pad)` / `(0,±1,pad)` — an exact sphere-vs-plane test the F frustum
machinery already provides, strictly tighter than a cone. For reference, the
**re-derived circumscribed cone** (had we mirrored Shadows.cpp's 90° form,
which the plan warns against copying): a square pyramid's corner sits at angle
`atan(√2·tanθ)` off the axis, so for the padded face `tanα = √2·1.25 =
1.76777`, `cosα = 1/√(1+2·pad²) = 1/√4.125 = 0.49237`, `1/cosα = 2.03101`
(vs Shadows.cpp's 90°-pyramid `√2 / 0.57735 / √3`). The 4-plane test is used;
these constants are recorded so the padded face is never conflated with 90°.

