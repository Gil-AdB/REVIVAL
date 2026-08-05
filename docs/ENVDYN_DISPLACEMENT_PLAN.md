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
- [x] **S1/S2/S3 — symmetric ADAPTIVE displacement (diagonal-grain fix). DONE
      (2026-08-01).** Replaces the B3 two-step (linear `SubdivideMaterialFaces`
      + `DisplaceMaterialVertices`) for the greets stone with ONE pass,
      `DisplaceStoneSubdiv` (`DEMO/MeshOps.cpp`). Fixes the PROVEN diagonal
      grain (commit 4633aeb): every stone quad = two triangles on one shared
      diagonal, so displacing the interior put a roof-ridge along every quad
      diagonal (uniform grain). Now each coplanar/UV-agreeing quad is paired
      across its longest edge and retriangulated as a symmetric 2^L grid whose
      relief cells become 4-triangle CENTRE fans (dome peak lands on a vertex,
      not a shared edge); monotone-slope cells keep a field-following diagonal,
      flat cells the shortest — orientation varies with content, no uniform
      grain. Lone triangles get symmetric barycentric subdivision.
  - **S1 (ridge kill) — VERIFIED.** User pose `FDS_GREETS_CAM=-4.99491215,
    2.2463572,-35.3489342,-0.0635922998,-0.116912566,-0.991104245` t=1867,
    amp 0.6. OLD path (HEAD 4788b69 `--greets_displace`) shows the repeating
    diagonal roof-ridges on the left wall; NEW path is organic stone, grain
    GONE. `--displace_viz` overlay shows per-cell radial fans / centre verts
    (X per relief cell), not uniform diagonals. A/B PNGs `/tmp/displace3/`
    (base / wip_amp06 / head_amp06 / viz_amp06). NOTE: `--no-parallax` is NOT
    usable to isolate — the greets height-map load is gated on `parallax()`
    (GREETS.CPP:1307), so `--no-parallax` skips the bake entirely; parallax
    stays ON (default) and the grain is gone in the shaded POM result too
    (the B4 residual carries the fine band; POM is per-pixel, not tied to the
    triangulation).
  - **S2 (adaptive depth).** Per-quad level 0..3 from the height map's
    refinement error under the quad's UV footprint (bilinear-vs-true at 9
    probes/cell; a level passes at ≤12% bad cells; `greets_displace_adapt`
    scales the epsilon, >1 = deeper). Measured at t=5780, 1920×1080, threaded,
    40 iters (`[STONE]` log + scene bench):
    | mode | rooms Lhist (L0/1/2/3) | mesh faces | frame ms | T-junc pins |
    |---|---|---|---|---|
    | adaptive (adapt 1.0) | 24/1/91/13 | 10233 | 60.685 | 209 |
    | uniform L2 (`greets_stone_subdiv=2`) | 0/0/129/0 | 10348 | 64.405 | 0 |
    Adaptive is **−3.7 ms and −115 faces** vs uniform-L2 while killing the
    grain: it spends L3 on the 13 busy block-edge patches and stays L0 on the
    24 flat mortar patches (single-run means, ±1-2 ms noise). `adapt` gain
    shifts the bulk monotonically: rooms L0 count 8 (adapt 2.0) / 24 (1.0) /
    56 (0.5). Level-boundary CRACKS closed by pinning the finer side's edge
    verts onto the coarser side's straight displaced segment — crack-checked
    at amp 1.5 / adapt 2.0 (mixed L0/L2/L3): deep clean relief, no gaps/holes
    (`/tmp/displace3/crack_stress`).
  - **FACETING fix — weld-aware smooth normals + tangents**
    (`DisplaceStoneSmoothNormals`, `greets_displace_smooth` default 80°).
    `MakeFacesIndependentByAngle` (30° crease) SPLITS every displaced cell
    whose neighbours tilt past 30°, so each cell shaded flat → visible
    faceting / disco-ball (user-reported). A flat-base-normal restore did NOT
    fix it (it left a per-triangle TANGENT split → normal-map/POM seam that
    survives continuous normals, measured at any amp). The fix re-smooths the
    displaced surface: a scene-wide position-bucket WELD of the material's
    corners averaging the DISPLACED face normals AND per-face tangents within
    `greets_displace_smooth` (area-weighted, Gram-Schmidt'd) — coincident
    verts share the whole TBN frame, so both faceting and the normal-map seam
    go. Angle < 90° keeps authored 90° wall corners hard; material borders are
    hard for free (bucketed per base surface). Editor `MeshOps_ResmoothSurface`
    machinery, scoped to init scene + material. Replaced the base-normal
    registry entirely.
  - **ACNE fix — single shadow-id for displaced walls** (`GREETS.CPP`, behind
    the `greets_displace` guard). ROOT CAUSE of the residual grazing hairlines
    (user: "acne only on wrong subdivision"): the greets shadow bake clusters
    ShadowMatIDs per **(material, PLANE)**. A flat wall is coplanar → one
    cluster → no self-shadow; displacement tilts EVERY facet onto its own
    plane → each facet gets a unique ShadowMatID → adjacent facets self-shadow
    → per-facet acne (the fan-X + cell outlines, worst at grazing). Proven:
    `--no-shadows` removes them, depth-bias (even 12288 slope) does NOT (it's
    the PolyId identity test, not depth acne). Fix = add `rooms`/`floor` to
    `kSingleShadowIdMats` when displacement is on (the same remedy the mummies
    use, documented there as "killing the per-facet self-shadow acne"); block
    relief shading now rides the normal map + POM + AO per pixel. VERIFIED
    clean at 3 poses (S1 frontal t=1867 + two grazing incl. the user's
    `-10,3,-45,0.7,-0.1,-0.7`) — `/tmp/displace3/{s1_fixed,graze_sid,graze2}`.
  - **`--displace_viz` now DEPTH-TESTS** (`drawLineZ`, `WorldAabb.cpp`):
    line view-z interpolated, compared vs `ZPage16` (enc `0xFF80 −
    zscale*z`, pulled 1% nearer so a wireframe ON its surface wins instead of
    z-fighting); occluded far walls no longer show through, so the looked-at
    wall's fan structure is readable. Falls back to draw-through when
    `ZPage16` isn't live.
  - **S3 (default amp).** Swept 0.3/0.45/0.6 at two poses (t=1867, t=1588);
    all grain-free. Kept the shipped default **0.3** (the user's approved
    B3 look); 0.45 for more presence, 0.6 bold. Not changed in code — a look
    call for the user. PNGs `/tmp/displace3/amp_*`.
  - New flags `greets_displace_adapt` (1.0) + `greets_displace_smooth` (80°),
    both greets cat, both read only when `--greets_displace` is on. Flags-off
    byte-null proven by IN-PLACE isolation (stash the 6 files, hold the
    concurrent tree fixed): greets momy2 close-cam `61f09196` identical with
    and without the change; city `37e62845`, fountain `51fff7cd`, render_gate
    3/3, wasm links. (Isolation needed because a concurrent agent's uncommitted
    FLD_READ/Material/Deferred work perturbs whole-binary hashes vs a clean
    HEAD — orthogonal to this change.)
- [x] **BLOCK-PITCH adaptive depth + honest metric + legible viz. DONE
      (2026-08-02).** User verdict at a NEW pose (gray-block wall left of the
      civax screen, `FDS_GREETS_CAM=-12.8872108,2.7451086,-52.5132828,
      0.991839349,-0.0319343321,-0.123432674` t=2145): "the viz is still not
      great, and the subdivision is worthless" — ON ≈ OFF, no per-block relief.
      Three coupled root causes + fixes (all in `DisplaceStoneSubdiv` /
      `WorldAabb.cpp`, behind the same default-OFF flags):
  - **Depth cap was absolute, not map-relative.** The S2 metric capped at L3
    (8×8 cells/quad). This wall's quads span 2-3 UV tiles (block pitch 0.25 UV
    = 4 blocks/tile), so L3 left cells 1-1.5 blocks wide → geometry carried no
    per-block relief. FIX: estimate the block pitch in TEXELS per material by
    gradient-sum AUTOCORRELATION of the height map (`EstimateBlockPitch`;
    measured 64×64 texels @ mip2 for `rooms`, 43×43 for `floor`), and drive a
    PER-QUAD depth cap so each quad's cell footprint ≤ (pitch / cellsPerBlock)
    texels — map-relative, so however many tiles a quad spans, its cells land
    at block scale. kMaxLevel raised 3→5 (safety cap; the per-quad block cap
    bounds it). `--greets_displace_cpb` (default **1.0** = one centre-fan dome
    per block, mortar recessed at the shared cell border; blocks pop at the
    lowest cost). ON now clearly ≠ OFF at the user pose — blocks pop, mortar
    recessed, self-shadowed (`/tmp/displace6/on_cpb1.0`).
  - **The refinement + viz metrics ALIASED.** Both probed a sparse fixed
    stencil (9-pt refine / 4-pt viz); on a coarse cell the probes fell on
    similar-height block interiors and MISSED the mortar → read "matched"
    while carrying no relief. FIX: both now scan the cell/triangle's FULL
    texel footprint (strided) at the bake mip, max |truth − carried|.
  - **Viz green wash → legible.** Old `--displace_viz=2` filled every triangle
    at α0.55 tinted by error/global-max — the single worst edge cell washed
    everything green. FIX: error is a pre-normalized ABSOLUTE fraction of the
    map's peak-to-valley relief; matched cells (|err|<15%) get NO fill (thin
    wireframe only), meaningful error fades in RED (under) / BLUE (over). Now
    reads as "where geometry fails the map" at a glance.
  - **COST — measured, honest (t=5780, 1080p, threaded, iters=25/40).** The
    depth increase is expensive; `_cpb` is the dial. Baseline (old L≤3, ~10k
    faces) frame p50 **56 ms**. cpb=1.0 (**43k** faces): **74 ms (+18)**, RNDR
    +6 / BAKE +9 (per-frame shadow re-raster) / XFRM +3; init lightmap bake
    2.1s→~6s. cpb=1.5 (54k): 79 ms. cpb=2.0 half-block (103k, crispest flat
    tops): **121 ms (+65)**, init bake 15s — hero close-ups only. Shipped
    default 1.0 (pops well, best cost); the shadow-bake + XFRM share would want
    the deferred B5 face-cull to go higher. Flag stays default-OFF so the demo
    is unaffected; this is the ON-experience cost, reported not hidden.
  - **No regression** of the fixed artifacts at the prior poses (frontal
    t=1867 yellow wall, grazing `-10,3,-45,0.7,-0.1,-0.7`): no diagonal grain,
    no faceting, no acne, straight mortar, clean corners (one slightly wavy
    mortar joint at grazing — the cpb=1.0 one-cell-per-block tradeoff).
    A/B `/tmp/displace6/`.
  - Flags-off byte-null: render_gate mirror+cone baseline-identical (halotest
    + fountain mismatch the OLD pins but are byte-== pure HEAD 8474205 —
    pre-existing pin drift, proven by stash-isolation); city `37e62845` exact;
    greets stable-pixel 5v5 byte-null by majority (MINE set2 == HEAD both sets
    = 0; one 114-px batch was the documented ±1-LSB "1-in-12" greets race,
    maxΔ=1); wasm links.
- [x] **ISOLATED TEST RIG `--scene-displacetest` + defect it exposes. DONE
      (2026-08-02).** `DEMO/DisplaceTest.cpp`: ONE flat 8×8 quad, ONE material
      with an 8-bit height map, running the EXACT production bake
      (`DisplaceStoneSubdiv` on material `dtest`, greets flags verbatim) — an
      analytically-checkable rig for "is the subdivision honouring the map?"
      without a 100-block multi-tile wall in a full scene. Height map via
      `FDS_DISPLACETEST_MAP=0`(4×4 synthetic blocks) `/1`(triangle-wave, smooth)
      `/2`(real `greets_wall_h.png`) `/3`(running bond, ½-block row offset);
      `FDS_DISPLACETEST_SPAN=N` tiles the map over N UV tiles (N·4 blocks/axis —
      span 3 ≈ the real multi-tile greets wall). `FDS_DISPLACETEST_DUMP=1` prints
      the `[DTEST]` metric matrix over (map, span) then renders the selected
      combo from 4 poses (frontal/45/grazing/edge-on **top-down silhouette**) ×
      3 styles (lit / viz-1 / viz-2) to `/tmp/displacetest_*.ppm`; default =
      interactive free-cam. `EstimateBlockPitch` exposed in `MeshOps.h` so the
      rig measures with the PRODUCTION estimator (no runtime-consumer change;
      render_gate 3/3, city `37e62845`, fountain `51fff7cd` byte-equal; wasm
      links).
  - **Estimator VALIDATED.** Synthetic 4×4 blocks → pitch **256×256 tex @ mip0 /
    64×64 @ mip2** (exact); running bond → **128×256** (the ½-block offset read
    as 8 vertical positions); the REAL `greets_wall_h.png` → **255×256 @ mip0** —
    exactly the 256-texel period measured independently. Triangle-wave "smooth"
    map → no pitch (fallback) and stays **L1** (verts 9 / faces 8) at span 1:
    smooth slopes correctly do NOT over-subdivide. (A plain LINEAR ramp instead
    over-subdivides — but only because it is a SAWTOOTH once tiled, discontinuous
    at the u=1 wrap; correct sampler behaviour, not a bake defect, hence the
    triangle-wave control.)
  - **DEFECT crisply reproduced — the default `cpb=1` DOMES every block; there
    are NO flat plateaus.** New honest metric = fraction of target FACES that are
    actually flat (3 verts share a level) vs sloped. Map 0, default flags:
    **FLAT-TOP frac = 0 %** (all 64 faces sloped — a centre-fan dome per block),
    viz-2 worst error **0.81 of peak-to-valley**, top-down silhouette a
    DOME/triangle wave (pointed peaks), NOT the square wave the map demands;
    viz-1 shows UNIFORM one-fan-per-block, not "coarse plateaus + fine groove
    edges". Raising density: `cpb=2` is STILL 0 % flat (each ½-block cell still
    straddles the mortar edge) — you need **`cpb=4`** (27 % flat faces) before
    real flat block tops appear on this map at mip 2, i.e. the shipped default is
    two steps too coarse to represent flat-topped ashlar. This is the geometric
    root of the t=2145 "subdivision is worthless / ON≈OFF" verdict: at cpb=1 the
    block IS displaced (p2v 75 % of amp·range) but as a pyramid, so grazing/frontal
    read as bumpy-but-not-blocky. Running-bond span 3 (closest real-wall analog)
    maxes out at L5 and still lands **17 % flat / jagged spiky top-down / dense
    viz-2 error** — max depth does NOT rescue flat tops; only flat-topped cells
    (higher cpb, or a plateau-aware cell instead of a centre fan) would. The rig
    is the standing regression check for any future flat-top fix.
- [x] **EDGE-ALIGNED TESSELLATION — the flat-top fix (2026-08-02, commit
      120ae7e + follow-up).** The rig-proven centre-fan DOME defect is fixed
      terrain-engine style: subdivision cell borders SNAP onto the height
      map's mortar-groove lines; block plateaus become single FLAT cells (2
      triangles, zero interior tessellation); the step down/up rides a narrow
      transition band (~1.25-texel shoulder pads at the bake mip). New flag
      `--greets_displace_edge` (default 1, read only under `--greets_displace`;
      0 = the legacy dome path for A/B).
  - **Groove detection** (per material, MAP space, bake mip): mortar lines =
    below-threshold runs of mean-height profiles (threshold = min/max midpoint
    of the bimodal field, wrap-aware, sanity-gated against the block pitch).
    Horizontal grooves from the per-row profile (global); vertical grooves
    from the per-BAND column profile (band = block row between two horizontal
    grooves) — running bond's alternating phases land naturally as per-band
    positions (the real `greets_wall_h` IS running bond: 4 h-grooves, 4 bands,
    4 v-grooves each at alternating phase; the floor reads 12 bands × 10-13).
    Wander is absorbed by the pads, not modeled — straight lines per band.
  - **Cell layout** (per quad, axis-aligned UV charts only): rows between
    h-groove lines — wide grooves get step/floor/step rows (flat mortar
    floor), narrow ones step/step around the centre; per-row column breaks
    from the row's band (step rows carry their band's breaks so vertical
    walls stay sharp through block corners; floor rows take both bands'
    union). Internal line vertex set = union of the two adjacent rows' break
    sets; rows triangulated by a two-pointer march — crack-free by
    construction. Matched-rect diagonals follow the height field. Params
    quantized to a 1/2048 lattice for cross-quad bit-identity.
  - **Kept**: the adaptive fan path for lone triangles, rotated/skewed UV
    charts, structure-free maps (smooth control stays L1) and the
    `greets_stone_subdiv` uniform baseline; the side registry generalized
    from per-LEVEL to per-PARAM-LIST with polyline pinning (level boundaries
    AND edge-vs-fan seams heal identically); authored-border zero-pinning;
    single-shadow-id acne fix; TBN smooth weld (80° keeps the new ~90° step
    edges hard).
  - **Rig ([DTEST])**: map 0 span 1 default flags: 64 faces / FLAT-TOP 0%
    (all domes) → **578 faces / 50% by count / 86% by frontal-projected
    area**, p2v 75% → **100%**, square-wave silhouette + crisp flat blocks,
    viz-2 plateaus unfilled. Running bond span 3: 3808 @ 17% → **7106 @
    53% / 84%**, clean 12×12 running-bond wall. Smooth control byte-
    unchanged. Real map: running bond reproduced, flat tilted slabs, no
    domes. NOTE the count-based FLAT-TOP metric ceilings near ~50%
    structurally (a square wave spends few LARGE faces on plateaus, many
    NARROW ones on steps); the rig now also prints the amp-invariant
    FRONTAL-PROJECTED-AREA fraction — the honest instrument (86% vs the
    old path's domes ~0%).
  - **Greets (all behind the default-OFF --greets_displace)**: rooms takes
    the edge path on 60/67 quads (real wall grid detected: 4 h-grooves ×
    4 bands × 4 v-grooves, running bond), floor on 10/11 (12 bands ×
    10-13); lones + the rest stay fan; edge↔fan seams healed by the
    generalized pin (455 T-junction pins). A/B at the three campaign poses
    (`/tmp/displace7/`): blocks pop with FLAT faces + straight recessed
    mortar; no diagonal grain, no faceting, no acne, no cracks; the gray
    wall (t=2145) noticeably crisper than the dome path.
  - **COST — measured, honest (t=5780, 1920×1080, threaded, iters=40,
    same session)**: flags-off 52.5 ms · dome path 42.9k faces / 72.5 ms ·
    edge path **86.6k faces / 107.0 ms**. Same ms-per-face curve as ever
    (74@43k, 121@103k measured in the cpb round) — the edge carve is not
    slower per face, it makes MORE faces: a true square wave needs ~24
    tris/block (flat top + step walls + corners) vs the dome's 4-tri fan
    that failed the silhouette test outright. The "fewer faces" hope held
    only against the EQUAL-FIDELITY baseline (cpb=2: 103k/121 ms and still
    no flat tops; cpb=4 unaffordable) — vs the shipped-but-wrong cpb=1 it
    costs +34.5 ms. The demo default (flag off) is unaffected; the
    ON-experience dial is `--no-greets_displace_edge` (72.5 ms domes) /
    OFF (52.5 ms). cpb is not read by the edge path (the groove graph sets
    the density); it still drives the fan fallback. Going faster wants the
    deferred B5 tile pre-reject + shadow-bake face cull, as before.
  - **Flags-off byte-null — proven, with a new documented trap.** Gates:
    render_gate 3/3, city `37e62845` + fountain `51fff7cd` byte-equal,
    wasm links. Greets (raw hash-majority is dead): stable-pixel gate at
    CONSTANT content, code A vs code B in one build lineage — head+this
    change vs pure head = **1 px** (the documented ±1-LSB residual race);
    fresh-build twin proof 0 px; fresh builds byte-reproducible per
    source-state. TRAP (cost 90 min, now in the traps memory): a binary
    built in a git WORKTREE chdirs to the worktree's OWN Runtime
    (`ChdirToAssetRoot` probes `<bindir>/../../Runtime`) and silently
    renders COMMITTED content — vs the main Runtime's user-uncommitted
    GREETS.FLD that the greets pin includes — faking a 150-186k-px
    "regression" that survived every code bisect (all arms 0-1 px once
    content was held constant). To gate a worktree binary, copy it INTO
    the target Runtime.
- [x] **S4c — the t=6097 SLIVER GAP (fold/inversion) + the LIGHT BLEED
      (shadow-id collapse). DONE (2026-08-03, worktree branch).** User repro:
      `FDS_GREETS_CAM="18.4499683,5.16043377,-57.6482239,-0.824408829,
      -0.544822097,-0.153357133"` t=6097 `--greets_displace` — a long thin
      see-through sliver at a displaced wall's top edge (z==0 scan: 52 px
      through the wall), plus light lancing along the seam.
  - **Gap root cause — NOT a border-pin miss (three hypotheses disproven by
    measurement):** position-coincident cross-material verts (census: rooms
    48 coincident / 0 newly pinned — the single-target-face rule already pins
    the whole outer ring), index-duplicated verts (16/3704, weld no-op), and
    hard-crease pinning (44 edges pinned, gap byte-identical) all ruled out.
    The gap is a FOLD: a narrow authored return strip (x=17.898, 0.127 tall)
    has verts whose Preprocess-smoothed normals diverge across the corner;
    mean-centred recession moves them 0.02 vs 0.135 world → faces twist past
    90° → the commit's authored-sign N flip leaves N opposing the WINDING →
    the Transform plane cull rejects them while they front the camera.
    PROOF: force-two-sided closes the sliver with no other change (52→1 px);
    recess-only clamp reproduces it, protrude-only closes it; all
    position-keyed edges at the crack are sealed (the geometry is watertight
    — it's a CULL hole, not an opening).
  - **Fix `--greets_displace_fold_relax` (default ON):** B1-style iterative
    fold relaxation in DisplaceStoneSubdiv — halve the displacement of every
    vert of a face whose displaced winding crossed its own BASE plane
    (g_disp·g_base < 0; convention-FREE — an authored-N criterion flattened
    the SceneBuilder rig, whose winding-vs-N convention is opposite to FLD
    content, and at first marked the healthy −1 mass = the whole carve),
    before the cross-patch heal. Population: 3,882/63k rooms + 62 floor faces.
  - **Sweep (z==0, 1920×1080, ON vs OFF):** repro 52→1 (the 1 px pre-exists),
    frontal t=1867 104→2, graze −10,3,−45 624→1, gray t=2145 793→0, floor
    graze 1728→1, t=100..5780 timeline 590/391/135/137/145/3870 → 144/2/3/2/
    0/4; opened=0 at every enclosed pose; the two vista poses' "opened" px
    hug silhouettes (OFF's twisted slivers jutted past the authored edge).
    Carve look unchanged at the campaign poses (frontal/gray/graze crops).
  - **Bleed root cause:** the acne fix's whole-material single-ShadowMatID
    collapse made every rooms-vs-rooms occlusion self-match in the deferred
    PolyId identity test — the shadow pass renders two-sided, so the occluding
    wall IS in the omni's cube, it just carried the receiver's own id → an
    omni behind a wall lit the next wall through it.
  - **Fix `--greets_displace_shadow_planes` (default ON):** PARENT-PLANE
    ShadowMatID inheritance — the bake stamps each emitted face with its
    parent (pre-displacement authored) plane's registry ordinal
    (MeshOps_StoneParentPlane; transient tag), and the greets clustering
    resolves it to the standard quantized plane key: one id per WALL (acne
    stays fixed within a wall — same semantics as the flat clustering) but
    different walls occlude each other again. Clusters 2133 (collapse) →
    2183 = exactly the flags-off flat count. The S1 proxy gets per-face
    plane-matched ids. A/B at the repro pose (no --ao_direct): the bright
    band lancing along the wall seam is GONE with parent-plane ids and
    UNCHANGED by the gap fix alone — the bleeding was the shadow identity
    skip, not (mostly) the geometric gap; --ao_direct remains a taste dial
    for per-groove micro-shadow only. Acne pose (−10,3,−45) stays clean.
  - **Also `--greets_displace_neighbor_pin` (default ON):** position-
    coincidence border pinning against non-displaced geometry (scene-wide
    1e-4 vert+edge grid), the class originally hypothesized. Inert on current
    content (census 0 newly pinned) but correct — proven by the new rig.
  - **Rigs (`--scene-displacetest`):** `FDS_DISPLACETEST_NEIGHBOR=1` — a
    2-quad wall whose interior mid-edge is coincident with a separate
    non-displaced lintel: mid-line max|disp| OFF=0.48 → ON=0.0000 (PASS).
    `FDS_DISPLACETEST_FOLD=1` — REAL greets_wall_h.png, the SHIPPED amp 0.3,
    corner-smoothed normals on a 0.15-deep return strip: inverted faces
    OFF=1 → ON=0 (PASS; classifier groups by the parent-plane stamps).
  - **Gates:** render_gate 3/3 vs committed baselines; city `37e62845` +
    fountain `51fff7cd` byte-exact; wasm links; greets flags-off structurally
    inert (clustering byte-path identical, clusters 2183).
- [x] **S5 — the GRAZING ZIGZAG (`--greets_displace_line_height`, default ON).
      DONE (2026-08-04).** User repro: `FDS_GREETS_CAM="-7.38721609,2.72471762,
      -50.8239441,0.817980111,-0.113630958,0.563911617"` t=2845 — a vertical
      mortar joint sawtoothing ±25 px at a grazing view. The initial diagnosis
      (independent mid-slope samples on DETECTED groove-line verts) was
      **partially wrong** and corrected by causal measurement:
  - **Mechanism (measured, causal):** the artifact wall's authored UV chart is
    a 4:1 TRAPEZOID (non-affine) → `edgeAlignedQuad` rejects it
    (non-parallelogram) → block-pitch FAN lattice, which cannot represent a
    few-texel groove; the smeared pseudo-carve (recessed lattice column at
    ~13-texel row pitch + alternating field-following diagonals) shears into a
    128-px-period chevron sawtooth under the ~20× grazing magnification.
    Making the strip's heights merely CONSISTENT does **not** straighten it
    (measured — bit-identical depth in the artifact region); zeroing just the
    strip's displacement does (zigzag tracer std 7.5→3.7, look intact). POM,
    fold-relax, per-face mips (`--mips` default 0) and rasterizer UV transport
    (perspective-correct per pixel) all exonerated.
  - **Fix (bake-time only, one principle — verts tracing a groove displace
    consistently):** (1) EDGE-aligned patches: every groove-line vert takes the
    line's rep = MEDIAN of the height field along that line (min at crossings);
    keeps the carve, kills along-line variance — this also straightens the
    long-known "wavy mortar joint at grazing" on the edge path (graze pose
    `-10,3,-45` t=1867: dramatic A/B). (2) FAN/LONE fallback patches: verts
    within 2.5 texels of any groove line pin to the groove's PLATEAU reference
    (median along an inset line pad+1.25 texels outside the groove — the pad
    lines themselves read mid-slope 0.37 vs true plateau 0.60 at mip2 blur):
    the fallback does not carve what it cannot represent; the joint reads via
    albedo/normal map/POM exactly as before. Plateau interiors untouched.
  - **Results:** repro zigzag lateral deviation std 8.33 px → 4.40 (straight
    no-displace reference 3.08); census rooms 28,533 edge-snapped + 700
    plateau-pinned, floor 8,570 + 308; fold-relax 3,882→3,818 rooms / 62→57
    floor, converges as before. Look poses (frontal t=1867 / gray t=2145 /
    graze t=1867): relief, block pitch, carve depth unchanged. Pinhole sweep
    (5 poses + 6-t timeline, ON vs OFF): absolute counts stay 0–4 px/frame;
    the ±1 px deltas are single-pixel triangle-junction coverage flicker with
    CONTINUOUS depth across (verified per pixel), not cull holes. Rigs:
    DTEST FOLD PASS (1→0), NEIGHBOR PASS (0.48→0.0000), metric matrix
    byte-identical (map0 578 faces 50%/86%, runbond span3 7106 53%/84%).
    Flag-off: [STONE] bake summaries byte-identical to pre-change (verts
    36500/68513 faces, 30472 displaced [-0.164..+0.033], fold census 3882/62).
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


---

# ADDENDUM 2026-08-06 — TESSELLATION RE-MEASURED: +54.5 ms WAS WRONG BY 7×

Status: **measured; two perf companions wired as `--greets_displace` defaults;
one mirror-clone duplication bug fixed; all byte gates pass.** The tessellated
arm (`--greets_displace`) was retired by the user on 2026-08-05 for cost — "we
are not going to use it anyway, it's way too slow" — on the number this plan
itself recorded: **86.6 k faces / 107.0 ms against 52.5 ms flags-off = +54.5 ms**
at t=5780. That number was already stale when it was quoted, and two further
landings have made it stale again. Re-measured from scratch on fog-wt tip:

**`--greets_displace` now costs +7.3 ms/frame at the worst pose, and it is
CHEAPER than the recess-shell arm at three of six review poses.**

## A1 — why the retirement number was inflated, three times

| # | landing | date | effect on the number |
|---|---|---|---|
| 1 | `9b6d70d` `--tile_bbox_cull` (default ON) | 2026-08-02 **15:05** | its own commit message measures the displaced arm **100.0 → 87.0 ms** and flags-off 47.5 → 47.0, i.e. the delta **−12.5 ms**. It landed **1 h 40 m AFTER** `120ae7e`, the edge-tessellation commit whose "107.0 ms" this plan quotes. The B5 face pre-reject that was declined on the B2 numbers had in fact shipped. |
| 2 | `a1f89d4` `--xfrm_soa_inline` (default ON, bit-exact) | 2026-08-05 | −2.0 ms off the main-view front end at this exact pose. |
| 3 | `799c808` faceless-mesh skip | 2026-08-06 | the retired Piramid parent was **84.3 % of the displaced arm's 6.83 M shadow verts/frame** (VISIBILITY_PLAN §9b) and 261 768 verts of every mirror clone. Shadow front end 23.07 → 10.67 core-ms; main-view transform −25.2 %. |

None of the three is a tessellation change. The arm was retired on the cost of a
bug plus two missing optimisations.

## A2 — the measurement

`--bench=scene@scene=greets,t=…,iters=12–20`, 1920×1080, 12 threads,
`--xfrm_prof`, arms **interleaved round-robin within one script** and reported
**min-of-arm** (the box carried load 7–25 all session from a concurrent agent;
p50 and mean are unusable, min-of-N is the only load-robust statistic and even it
should be read as an upper bound). Per-section numbers are the profiler's own
per-frame minima. Load is stated with every table.

`flat` = `--deferred` (the shipping arm — flat POM, no geometric relief).
`recess` = the standing shell arm from S1D_CLOSED_SHELL_PLAN "Current standing arm".
`tess` = `--deferred --greets_displace` (which now defaults its two companions on).

### The headline, t=5780 (the historical bench pose), min-of-4, load 19–25

| arm | frame | Δ vs flat | RNDR | BAKE | XFRM | SORT | main verts | fPushed | baked faces |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| flat POM | **48.54** | — | 44.94 | 2.67 | 0.409 | 0.062 | 49 783 | 7 549 | — |
| tess, companions OFF | 61.77 | +13.23 | 53.46 | 3.35 | 3.524 | 0.606 | 434 668 | 74 962 | 87 256 |
| **tess, as it now ships** | **55.86** | **+7.32** | 49.45 | 3.38 | 1.787 | 0.296 | 253 280 | 36 147 | 87 256 |
| tess, dome path (`--no-greets_displace_edge`) | 55.64 | +7.10 | 49.46 | 3.32 | 1.896 | 0.297 | 225 496 | 37 070 | 42 924 |
| tess, uniform L2 (`--greets_stone_subdiv=2`) | 52.56 | +4.02 | 48.16 | 3.18 | 0.657 | 0.104 | 75 271 | 12 731 | 10 268 |
| tess, uniform L1 (`--greets_stone_subdiv=1`) | 50.01 | +1.47 | 46.04 | 3.06 | 0.483 | 0.071 | 55 153 | 8 640 | 6 522 |

**+54.5 → +7.32 ms is a 7.4× reduction on the same pose, the same content and
the same amplitude (amp 0.3, the reviewed look).** The bake is bit-identical to
the S5 state (`rooms` verts 36500 / faces 68513, 30472 displaced
[−0.131..+0.033], fold census 3818/57) — nothing about the tessellation changed.

Note the fourth row: with the companions on, the **edge-aligned carve (87 k
faces) costs the same as the dome path (43 k faces)** — 55.86 vs 55.64. The
"+34.5 ms for the edge path" this plan recorded is gone; the correct-looking
carve is now free relative to the wrong-looking one.

### Per pose, min-of-5 (min-of-2 at t=5534), load 15–19

| pose | what it is | flat | recess | tess | tess−flat | recess−flat | **tess−recess** |
|---|---|--:|--:|--:|--:|--:|--:|
| 5743 | wall fills the frame | 48.80 | 55.50 | 62.55 | +13.75 | +6.70 | +7.05 |
| 5780 | wall + statues bench | 48.98 | 56.05 | 62.90 | +13.92 | +7.07 | +6.85 |
| 5958b | grazing, near-parallel | 44.44 | 53.71 | 58.55 | +14.11 | +9.27 | +4.84 |
| 6097 | corner close-up | 38.51 | 44.60 | 42.56 | +4.05 | +6.09 | **−2.04** |
| 2845 | grazing close-up | 46.53 | 51.84 | 50.51 | +3.98 | +5.31 | **−1.33** |
| 5534 | corridor, looking back | 46.04 | 52.63 | 48.47 | +2.43 | +6.59 | **−4.16** |

Mean tess−flat over the six poses **+8.7 ms**, worst +14.1. These are with the
companions OFF (they were taken before the defaults landed) — subtract ~5.9 ms
at the wall poses for the shipping arm. **Tessellation is cheaper than the
recess shell at half the review poses**, because the shell's cost is per-PIXEL
(it scales with how much wall is on screen at how grazing an angle) while
tessellation's is per-FACE and largely pose-independent.

## A3 — where the remaining cost is, and the two companions

Decomposition of the +13.23 ms (companions off) at t=5780: RNDR +8.51, XFRM
+3.12, SORT +0.54, BAKE +0.69. Two levers, both measured, both existing
machinery:

**(1) `--greets_shadow_proxy` — the offscreen bake. −5.9 ms.**
The flag already existed (default 0, "a look call"). Re-measured on the
post-`799c808` tree: frame 67.95 → 62.05, **BAKE 9.18 → 3.45** where flat POM's
own BAKE at the same pose is 3.26 — the proxy removes essentially the whole
displaced-caster bake. **Not look-neutral, and not claimed to be:** over the 16
review camera/t pairs it is byte-identical at 5 and moves 1 282–110 668 px
>4/255 elsewhere, worst t=6097 (58 021 px >12/255, 14 604 >32/255, max 144, mean
|Δ| on changed px 1.0–4.3), concentrated at the corner junction where flat
casters stop occluding. No structural artifact in the crops.

**(2) `--greets_displace_flat_mirror` (NEW) — the mirror clone. −5.9 ms.**
The finding that made it worth building: **`GreetsMirror` clones the scene as
MAIN-VIEW geometry, and `Face_MainOnly` is only honoured in OFFSCREEN passes**,
so the reflection re-transformed and re-rasterised the entire tessellated wall.
Measured at t=5780 with `--no-greets_mirror` as the ablation:

| arm | with mirror | no mirror | clone cost | of which |
|---|--:|--:|--:|---|
| tess + proxy | 62.90 | 51.50 | **11.40 ms** | RNDR 7.19 + XFRM 2.17 + SORT 0.34 |
| flat POM | 48.98 | 45.67 | 3.31 ms | RNDR 2.99 + XFRM 0.21 + SORT 0.03 |

The clone **pushed 42 870 faces while the direct view pushed 28 598** — the
reflection drew more displaced wall than the camera did, because the clone is
culled by the frustum and not by the mirror WINDOW (0.04–3.9 % of screen). This
is §8d's "clone the flat proxy" proposal, which §9e correctly voided for the
FLAT arm (nothing to trade there) and which is emphatically not void here.
With the flag on: frame 61.77 → 55.86, main verts 434 668 → **253 280 (−41.7 %)**,
fPushed 74 962 → **36 147 (−51.8 %)**, **XFRM 3.524 → 1.787 (−49.3 %)**, SORT
0.606 → 0.296. Look cost: **byte-identical at both mirror review poses (t=6133,
t=6293)**; at t=5743 2 990 px >12/255 (0.14 % of the frame, inside the mirror
window) and the two frames are indistinguishable side by side. The reflected
masonry keeps albedo + normal map + the POM march (§9e measured the march reads
in the mirror: 96.7 % of its 32 674 px move without it) and loses only the
geometric block relief.

**Both are now `setDefault` under `--greets_displace`** (GreetsApplyInitDefaults,
DEMO/GREETS.CPP), so ONE flag gives the affordable arm, and a `[STONE]` line
names them and their look cost in every run's log. `--no-greets_shadow_proxy` /
`--no-greets_displace_flat_mirror` still win. Both are inert without
`--greets_displace`, so the shipping flat-POM arm is untouched — proven by the
gates below.

**BUG FIXED on the way (FDS/RENDER/GreetsMirror.cpp).** `--greets_shadow_proxy`
alone put the wall in the reflection TWICE: the clone builders skip nothing on
`Tri_OffscreenProxy`, so they copied both the displaced faces (whose
`Face_MainOnly` offscreen tag does not apply to a main-view clone) and the flat
proxy standing in for them — two coincident copies, Z-fighting. Measured
+148 clone faces pushed per frame at t=5780 (74 962 → 75 110); with the fix the
count is 74 962 again. All four clone loops (base count/fill, compound
count/fill) now share three helpers, `mirrorFlatStone` / `mirrorSkipProxyMesh` /
`mirrorSkipFace`, and the mesh-level vert skip is flag-gated so the count is
bit-identical off the flag.

## A4 — the S2/S5 chunk LOD: ceiling MEASURED, and it is small. Not built.

The plan's never-built S2 item (per-chunk screen-space-error LOD) was the
assigned lever. Its prize is now bounded empirically rather than estimated,
using the face-count ladder in the A2 table as a stand-in for "every chunk one
level coarser":

* RNDR delta from 6 522 faces (uniform L1) to 87 256 (edge) is 46.04 → 53.46 =
  **7.42 ms for 80 734 faces = 92 ns/face threaded ≈ 0.60 µs/face core** at the
  ~6.5× pool speedup. The B2 spike measured **2–2.8 µs/face serial** for the
  same fixed cost. **The user's instinct is right and this is the number: the
  per-face cost is 3.3–4.7× lower than the figure the campaign has been
  reasoning with**, almost all of it `--tile_bbox_cull`.
* Halving the faces (edge 87 k → dome 43 k) is worth **−6.13 ms with the
  companions off**, but the companions have already taken the clone half of that
  work away: with them on, edge (87 k) and dome (43 k) measure **55.86 vs 55.64
  — 0.22 ms apart**. A LOD that halves the face count therefore buys **≈ 0.2–3 ms
  at this pose**, against a bake-time-only ladder (the bake is 2–6 s, so
  per-frame re-tessellation is out), 25–40 MB of resident levels, a per-frame
  swap, and DMM's per-edge min-level rule to keep chunk boundaries watertight
  when neighbours pick levels independently — the existing border-pin machinery
  closes cracks for a FIXED level assignment only.
* Verdict: **not built, deliberately.** Spending the session's remaining budget
  on ≤3 ms with a crack risk and a relief-fidelity cost, while 11.8 ms of the
  original 13.2 was sitting in two existing-machinery flags, would have been the
  wrong trade. The ladder above is the evidence, and it is reproducible in three
  bench runs if the priority changes.
* What §9f already refutes for this arm: `--greets_shadow_proxy` is NOT
  redundant here. §9e measured wall casters at 2.3 % of the post-fix shadow front
  end **in the flat arm**, where the walls the bake rasterises are already the
  ~226-face flat surface. Under `--greets_displace` they are ~87 k faces and the
  proxy is worth 5.9 ms — the "that win is already banked" line is scoped to
  flat POM.

## A5 — the look, on current HEAD, at every review pose

Rendered `--greets_displace` at all 16 camera/t pairs in
`docs/greets_review_poses.txt` (13 review poses; 5958 has four). **It still
looks right after everything that has landed** — the AO-map fix, PBR by
default, the handedness fix `7bfbc87`, the groove-line fix, the faceless-mesh
skip:

* t=5743 (the 2026-08-04 "black gashes" primary repro): real block relief with
  true silhouettes on the block edges, deep straight mortar, **no gashes, no
  acne, no faceting**.
* t=5958b (the 2026-08-05 grazing-smear repro): the pose where the recess shell
  smears. Tessellation renders a crisp geometric step with a real silhouette;
  the three-arm crop is unambiguous — flat POM shows a thin dark line, the
  shell a smeared band, tessellation an actual edge. **Geometry cannot swim, so
  the whole slip/cap ladder that dominates the shell campaign does not exist
  here.**
* t=2845 (the S5 zigzag repro): mortar joints straight, no sawtooth.
* t=6133 / 6293 (mirror panels): the reflection is correct.
* t=6097 (corner close-up): no sliver gap, no light bleed — S4c holding.

**And the honest failure, re-verified today: §C4 reproduces.** Same method as
S1_DISCREPANCY_INVENTORY §C4 (least-squares fit of 1/z over one authored wall
plane, residual in world units), fresh `FDS_SNAPSHOT_ZDUMP` captures, zscale
395.6364:

| box | flat POM + `--pom_depth_write` | recess shell | tessellation |
|---|--:|--:|--:|
| t=6097 (1150,120)-(1750,520), 240 000 px | 0.0233 | 0.0110 | **0.0023** |
| t=2845 (660,340)-(1260,740), 240 000 px | 0.0536 | 0.0524 | **0.0444** |

0.0023 world is one zEnc code (0.00253). **At t=6097 the tessellation writes NO
measurable relief in that 600×400 region** — §C4's finding, unchanged: the
region is one tessellation triangle, and the carve lives only at the lattice.
At t=2845 the same arm carries 83 % of the depth-writing per-pixel arm's span.
The class is strongly pose- and quad-dependent, and it is tessellation's own
error, not a yardstick.

**PROVENANCE NOTE.** The `greets_displace_flat_mirror` line in
`FDS/Base/FeatureFlags.def` was swept into a concurrent session's commit
`897e6f5` ("viz: runtime debug-viz cycle (X / Shift+X) + whole-scene wireframe
overlay") while this work was in flight — same worktree, broad `git add`. The
commit title does not describe the flag it carries. Nothing is lost; the log is
misleading. Same class of accident as the `3712f00` / `2c54ae9` note in
S1D_CLOSED_SHELL_PLAN §S1d-2.

## A6 — gates

Taken with these changes AND a concurrent agent's uncommitted work in the same
binary (the documented shared-tree situation), so read them as "this tree is
byte-null on the shipping arms":

| gate | result |
|---|---|
| city @t=1961 | `37e62845c4d30eefa321730c5bb7e0b8` **exact** |
| fountain @t=2500 | `51fff7cd38767d619280afe0498a6f24` **exact** |
| greets @t=1588 (pin recipe) | `06e1d4d1bf175f6de40707df094924c9` **exact** (the §9d pin) |
| render_gate mirrortest / conetest / halotest | **3/3 PASS**, all == baseline |

Not gated: t=6097 under `--greets_displace`, which OPTIMIZATION_BACKLOG records
as run-to-run nondeterministic (6 runs, 6 hashes) while t=5780 is stable 6/6.
**Not root-caused here** — it was not cheap to reach and nothing in this work
touches it. It remains the one thing standing between the tessellated arm and
full gate-worthiness.

## A7 — the choice, as a matrix (Phase C)

Cost = frame ms at t=5780, min-of-arm, same script, same box. "Relief depth" =
the §A5 1/z-residual span in world units at the two boxes (t=6097 / t=2845);
0.0023 is one zEnc code, i.e. none.

| | flat POM (ships) | recess shell | **tessellation** | lid shell |
|---|---|---|---|---|
| cost @t=5780 | **48.5** | 56.1 (+7.1) | **55.6–55.9 (+6.2–7.3)** | ≈ recess |
| cost @grazing (5958b) | 44.4 | 53.7 (+9.3) | 58.6 (+14.1) | — |
| cost @corner (6097) | 38.5 | 44.6 (+6.1) | **42.6 (+4.1)** | — |
| cost @corridor (5534) | 46.0 | 52.6 (+6.6) | **48.5 (+2.4)** | — |
| relief depth written | 0.0233 / 0.0536 *(only with `--pom_depth_write`, default OFF → 0.0023)* | 0.0110 / 0.0524 | **0.0023 / 0.0444** | — |
| **true silhouettes** | no | **structurally impossible** (recess-only cannot protrude) | **YES** | claimed; see below |
| swim / slip | offset-limited, mild | **p90 15.3 / p99 501 at cap 64**; clean only at cap 2–4, where reach ≈ flat POM | **none — geometry cannot swim** | worse |
| offscreen consumers (shadow bake, env probes, mirror RTT, lightmap bake) | flat everywhere | flat everywhere (raster-side only; shadow cube vs flat = **0**) | **real geometry everywhere**, or the flat proxy by choice (shadow cube vs flat 5.32 %) | **18.77 % shadow-cube divergence**, 9 033 non-stone px >12/255 at p5743 |
| see-through between blocks | no | **no** | **yes, and it is real geometry** — but greets is a closed room, so there is nothing behind a mortar valley to reveal (measured 0–23 px/pose even at 3.3× amp) | not demonstrated |
| where its error lives | everywhere (no depth, no silhouette) | grazing smear + the clamp band | **only at the lattice**: whole quads carry zero relief (§C4) | void + offscreen |
| gate status | pin `06e1d4d1` | byte-identical flags-off, 13 poses | **t=5780 stable 6/6; t=6097 NONDETERMINISTIC 6/6 distinct — open** | as recess |

**The contrast that decides it.** Tessellation and the shell arms fail in
*opposite* places. The shell is exact where it has pixels to march and wrong at
grazing and at silhouettes; tessellation is exact wherever its lattice reaches
and simply absent where it does not. Their costs are now the same order — and at
three of six review poses tessellation is the cheaper one — so cost is no longer
the discriminator it was assumed to be. What remains is a **fidelity** choice
between "relief everywhere, approximate, flat-edged, and it swims at grazing"
and "relief at block scale, exact, real edges, and some quads carry none".

The two are also **composable and already wired that way**: under
`--greets_displace` the POM input is swapped to the B4 residual, so geometry
carries the block band and the march carries the sub-block band. That is the S2
ladder the research doc shortlisted, minus the per-frame LOD — and §A4 measures
the missing LOD as worth ≤3 ms.

**Not measured here, stated as such:** the lid-shell column is copied from
§S1d-2d (2026-08-05) and was not re-run; the flat-POM relief-depth row needed
`--pom_depth_write`, which that arm does not ship with, so its shipping
configuration writes 0.0023 like tessellation's worst box (§C5).
