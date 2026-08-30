# Displacement bake v4 — design

Status: DESIGN, no code. Written 2026-08-30 on `rev-bakedesign` (off `fog-wt` fcca1ed7)
by the coordinator's design fork, after Gil-Ad's decision to rewrite the bake from
scratch (ledger decision `f2696d3c8aa7`, verbatim quote inside). Every choice below
cites the ledger record that measured it; `groundwork query <subject>` reproduces
the citation. Three choices are marked **PENDING HIS RULING** and carry the
assumption taken meanwhile plus its falsifier.

Companion documents: `docs/DISPLACEMENT_LITERATURE.md` (the survey, §A–G +
addendum), `docs/DISPLACEMENT_RESEARCH_II.md` §REF / §REF-2 (the reference
renderer, two rounds), `docs/BULGE_CORPUS.md` (his verdicts, states 1–12).

## 0. What v4 is, and when it is done

v4 is a new `DisplaceStoneSubdiv` that **generates the same solid the reference
renderer marches** — the union of per-face offset slabs of the authored stone
planes — as a tessellated, watertight mesh, instead of patching the 1998 mesh
with a lattice and a chain of normal-side corrections. It runs once at scene init
behind `--greets_displace_v4` (default OFF); the old bake stays the shipped
default until v4 replaces it by his fly-through. The reference renderer
(`--greets_displace_ref`) is the yardstick, not the goal: v4 is judged by its
distance from that definition, per pixel, and by his eye.

Acceptance (all measured, all at the corpus poses cam A / cam B / H6017 / H6194
and the 22-pose review list, `docs/greets_review_poses.txt`):

| criterion | target | instrument | today (old bake) |
|---|---|---|---|
| geometric error vs the reference | dz p50 within 2× the reference's own noise floor (the bare-arm p50, 0.016–0.035 u per pose), p90 < 0.08 u, no pixel > 0.16 u (the amplitude) | `tools/refrender_diff.py` (`7ac873df0332`, `f47faa153876`) | p50 0.026–0.071, p90 0.06–1.22; bake is further from the definition than the FLAT wall at 7 of 8 poses |
| holes | **0** hole px on the 54-pose tear battery, every pose | `tools/tear_battery.sh` + `tools/tear_detect.py` | 2 169 (rev-dispfix tip), 5 454 (fog-wt) |
| shading normal vs the reference | normal-angle p50 < 5°, p90 < 15° on stone pixels | `refrender_diff.py` normal plane | p50 4.7–21°, p90 40–114° |
| his eye | fly-through at the corpus poses reads at least as well as state 11 ("C looks good") | his words, recorded by the coordinator | `6e423128aa33` |
| init | bake wall-clock ≤ 500 ms (old bake: 475–530 ms) | `[STONE-*]` banner timing | 523–534 ms |
| faces | ≤ the old bake's at equal relief (~85–90k scene faces); no sliver family: min-angle p10 > 2°, no face under 1°; corner-column density ≤ 2× the wall's | census of `displace_faces.txt` | 73 vs 11 faces/u² at the x=17.898 column, 154 of 400 faces under 1° |
| per-frame | greets tick t=5743/5965 min-of-11 within the ±1.5–2 % placement floor (`perf.floor` law, §00w) | `--bench=scene` protocol of PERF_LAWS.md | — |
| old default | byte-identical with the flag OFF, every pin | render_gate / warm_gate / 14 pins | — |

## 1. The model v4 generates

The definition is the reference renderer's (§REF "The model it renders"), with the
two corrections its own pictures forced (§REF-2). Each element, and what measured it:

1. **Height field.** `d(u,v) = amp·(h(u,v) − mipMean)`, `h` bilinear at texel
   centres, toroidal wrap, at the bake mip (mip 2; mipMean 0.5471 rooms /
   0.3405 floor; amp 0.300; |d|max 0.164 rooms / 0.050 floor). Same convention as
   the reference so that dz measures the *mesh*, not a sampling difference.
2. **Direction = the face's authored plane normal.** Nothing else, anywhere.
   Law `420fcd4626b8`; the survey's OpenSubdiv quote (§B) and the month's root
   cause: the smoothed 1998 normal reached the geometry through three doors
   (mitre bisector dd798c31, groove-shade target 41ff72ed/16aac2ee, ride sign
   a0c46c40) and each door was a separate bug. v4 has no such field to pollute.
3. **Slab per face.** Signed distance from the face plane in `[−back, d(u,v)]`,
   over the face polygon **extended past each shared edge** by the per-edge
   margin (item 5) and **trimmed at the mitre only at convex edges** (item 6).
4. **Membership = UNION.** A point is solid if *any* slab contains it. The
   literal "nearest plane governs" partition **deletes material** at grazing
   corners (trap `e43c035dbedf`: face 881 edge-on erases ~90 px of the pier
   front at cam A). **PENDING HIS RULING** (open `5a45f66b1478`, ambiguities
   `5463f141eb8f`); assumption: union. Falsifier: a corpus pose where the
   partition arm reads better to his eye AND deletes nothing (the reference has
   both arms: `--greets_displace_ref_partition`).
5. **Extension margin, per edge, derived — never a constant.** A crease with
   normal angle φ needs `max(cot, tan)(φ/2)·|d|max` (cot is the bisector's need,
   tan the mitre's; the survey's cot correction, §G addendum). A **smooth seam**
   (φ below the crease threshold) is not a corner: sizing it by cot replaced the
   curved wall (trap `60e3e63bed65`). Margin capped at `sqrt(L²−1)·|d|max` for
   mitre limit L = 4 (SVG's default; fires on 1 edge in the scene — every real
   corner here caps at 2× the height, law `f713599ea11d`).
6. **Mitre trim at CONVEX edges only.** Applying the trim at every shared edge
   turned the union into an intersection and produced 97.5 % of the reference's
   own holes (`032cf9150e6a`: 4 007 of 4 009 misses at cam A vetoed, 0 from a
   convex edge; nine-pose misses 31 307 → 773, `582c083abef6`). Concave
   junctions (wall meets floor) need no rule under union.
7. **Free edges → skirts, after classification.** A free edge is used by exactly
   one face with no face of *any* material sharing it or position-coincident
   across it. The scene has **4**. Everything else is shared, and the survey's
   rule stands: at an ambiguous edge the safe default is "shared — weld", never
   "open — skirt" (a skirt on a misclassified edge hides the real crack, and the
   floor-base tears were exactly a material-blind misclassification, bc79e39d).
8. **The corner rule is PER JUNCTION, decided by the crease census** — this is
   the design's answer to his "the height doesn't always agree from both sides"
   (`a0e7dfd23ca4`). The census (`83aa7c0b6c4c`; §REF-2) over 453 junctions,
   7 375 u of crease: **73.3 %** of the length is the *same* relief 1.4 texels out
   of phase (r_best ≥ 0.8) — a dominant-owner height on the crease reconciles
   it; **21.2 %** is two *unrelated* UV charts (r_best < 0.4, no lag helps) — no
   corner rule reaches it; 5.5 % in between. So:
   - phase-shifted junction → **dominant height on the crease** (Dudash's
     dominant data, micro-mesh's duplicated edge values; owner = canonical
     order-independent key; blend back over 2 texels). No step. Exact in band:
     the reference's shared-edge arm proves the band is a closed-form t-interval
     (`75a1ca5fcd65`: 505 ms, hit mask bit-identical).
   - unrelated-chart junction → the options, none free: (i) **steps** — his
     ruling #1 of 2026-08-29, a castellated bisector step; both sides keep their
     own height; honest to the data, reads as artifact where the courses do not
     line up; (ii) **dominant** — picks a winner, hides the other wall's course
     for 2 texels; (iii) **re-author the UVs** at those junctions — the only fix
     that makes the two sides agree in fact; the census names them (junction
     centroids in `docs/img/refrender/`, e.g. the four tiled room modules at
     (4.9, 2.5, −37.0) and its copies).
   **PENDING HIS RULING**; assumption meanwhile: dominant on phase-shifted,
   **steps** on unrelated (his standing ruling), both as one flag
   `--greets_displace_v4_corner=census|steps|dominant`. Falsifier: the
   junction-pair renders (`docs/img/refrender/junction_<pose>.png`) read worse
   to his eye under the census rule than under either uniform rule.
9. **Shading normal** `n ∝ N − ∇ₛh` in the plane's own frame — exact on a plane
   (survey §C); split normals at creases (dihedral > 30°); a smooth base normal
   only inside a merged chart (the curved wall). Never an average across a
   crease (the pillow shading, 16aac2ee; the crease-gate refutation
   `864d9658ae3a` was a gate on the *wrong* field — v4 has no averaged field).
10. **Back and lid.** A slab's back is the amplitude (§REF ambiguity 4); a face
    whose lid is authored (the pier top) is a face like any other — its slab
    extends past its edges by item 5 and meets the wall slabs by item 6 (this is
    the lighter wedge he saw at the pier top in `camA_triptych.png`; it is the
    definition, and the census rule decides whether that crease is dominant or
    stepped).

What v4 deliberately does **not** carry from the old bake: the profile
densification at pitch 0.08 u along every border line (the source of the 73
faces/u² corner column and its 154 sub-1° slivers), the line-height rep
machinery (E/P levels, crossing-minimum rule), the free/pin/recess vertex
classes, the reflex weld, the joint snap/split family (all refuted:
`408a38faafa7`, `09cecbe6d510`, `baf27927b0e5`, `0c2cd2a858e6`, `c165bff2f03a`,
`20be1fac463e`), `cpb` as a wall lever (`558b45ea139b`), and every flag that
chose a sign by dot with a smoothed normal.

## 2. The pipeline, stage by stage

Each stage states its **invariant** and the **instrument** that checks it. A stage
that cannot be checked in isolation is not a stage. Every instrument is a
`--v4_census` print plus a `tools/` reader, so a round can gate on it.

### (a) Stitch and half-edge over the authored soup
- Input: the authored stone faces of every mesh (`rooms`, `floor`, and their
  `::mirUV` clones — the clones are what the main view rasterises, §REF "five
  things"; v4 must bake the same set the old bake baked, which is decided by the
  `::mirUV` split at `GREETS.CPP` ~2868 running AFTER the bake at :2019–2020;
  v4 runs at the same point and bakes the same meshes, then the clone split
  copies the baked result — that ordering is kept, not re-derived).
- **Exact-equality stitch** (bitwise / single-ULP hash) — authored coordinates are
  bitwise equal at shared corners (survey §E). ε-welding only as an instrumented
  fallback that prints what it merged, ε relative to the shortest authored edge,
  never in world units (the old bake's 0.006/0.02 world-unit welds and their
  chaining: `0c2cd2a858e6`, `20be1fac463e`).
- **Half-edge** with boundaries as null *face* (never null twin). Material and
  coplanarity are **attributes** of the two faces at an edge, never topology: an
  edge with two faces of different materials is a material seam, not a boundary
  (the base-junction bug, bc79e39d: `abutPointMat` excluded the floor face as
  "the edge's own face"; the trap `greets.wall.bake_face_array_is_whole_trimesh`
  is the same class).
- Edge endpoints ordered by **world position** before anything is derived from an
  edge (DiagSplit's precision rule; §REF "five things" #5).
- Invariant: every edge has use-count 1 (genuine boundary — expect the scene's
  4 free edges plus doorway/lid silhouettes, each listed) or 2; zero non-manifold
  edges; zero near-coincident vertex pairs closer than ε that are not the same
  vertex. Instrument: `[V4-STITCH]` census (counts by class, the list of
  use-count-1 edges with their world segments) and `tools/v4_stitch_census.py`.

### (b) Plane registry (charts)
- VSA-style region growing over faces with a **normal budget** (the survey's
  L^{2,1} proxy; the optimal proxy normal is the area-weighted face normal): the
  curved wall's narrow strips merge into one chart; hard corners split. Registry
  is **material-blind** (a wall and the floor are two planes of one solid) and
  built across ALL meshes — the old `envDoms` block (`MeshOps.cpp` ~2082) is the
  precedent and is reused read-only.
- **Two thresholds, two questions** (survey §F/7): chart membership (normal
  budget, ~10°) decides *parameterisation*; the crease threshold (30°) decides
  *shading*. They are not the same predicate.
- Invariant: every stone face belongs to exactly one chart; the chart's plane
  fits all its faces within the budget; junction list = every pair of charts
  sharing an edge, each with φ, convex/concave/smooth class, and the crease
  census numbers (item 1.8) attached. Instrument: `[V4-CHARTS]` (chart count,
  faces per chart, max deviation, junction table) — expect ~24 dominant planes
  plus the curved-wall chart.

### (c) Per-chart UV-aligned lattice
- Domain: the chart's UV space (all its faces share a chart because they share a
  chart of the height map).
- **Breaklines** on the mortar centrelines and the block-edge pairs: the
  centrelines come from the height map's row/column profiles (the old bake's
  line machinery found them; v4 keeps the *finding*, drops the rep levels).
  Constrained Delaunay within each block cell; slivers *along* a mortar line are
  legitimate (Dyn–Levin–Rippa), slivers *across* one are not.
- **Plateau nodes ≥ 4 level-0 texels inside the block** (the shoulder mechanism:
  mip-2 bilinear smears a block edge over ~8 level-0 texels; a node on the
  boundary reads the plateau/groove midpoint — measured as the −0.036 u plateau
  bias, `d8e1d26bfc3e`; `shoulder_plateau` was a hand approximation of this rule,
  `40ac61208dfa`).
- **Density: restricted quadtree** — adjacent cells differ by ≤ 1 level (Von
  Herzen & Barr); block interior cells at the `cpb` density, groove bands one
  level finer, never a 0.08 u profile pitch against a 2.5 u interior (the
  73-vs-11 faces/u² fan at the corner column).
- **R1/R2/R3 on every shared border**: the border owns its sample count (derived
  from the border alone, never from either chart's interior); parameters are
  exact `i/n` integers; each border vertex is created **once** and indexed from
  both charts (the old bake's `edgeVert` keyed on float bits and minted twins —
  `0c2cd2a858e6`). FMA contraction: border positions are computed in one function
  from one canonical operand order with `-ffp-contract=off` on that translation
  unit (trap `607c7cd2c810`: `-ffp-contract=fast` contracted a duplicated
  expression differently and moved two pins).
- Invariant: watertight **before displacement** (law `7453a529070e`): use-count
  2 on every interior edge, zero T-vertices (no vertex lies on an edge it is not
  incident to), and the undisplaced v4 mesh renders **byte-identical** to the
  bare wall at every pin (amp = 0 ⇒ the same planes). Instrument: `[V4-LATTICE]`
  + the tear battery on the undisplaced arm (must read 0) + the pins.

### (d) Heights
- **Placement from the mip-2 field, values from the pyramids**: plateau nodes
  read the **max**-pyramid, groove nodes the **min**-pyramid, bevel nodes the
  bilinear value (Garland & Heckbert §3.2; Tevs 2008). The reference samples the
  mip-2 field pointwise, so v4's dz at plateau interiors measures this rule
  directly: target plateau bias 0 ± the reference's noise.
- Invariant: per relief class, e−r along the plane normal p50 within ±0.005 u
  (`tools/nspace_relief.py`, the instrument that found the −0.036).

### (e) Displacement and junction rings
- Every interior node moves along the **chart plane normal** by `d(u,v)`.
- **Junction ring vertices** (shared by two charts at a crease) are placed by the
  offset-plane solve `u = d·(n₁+n₂)/(1+n₁·n₂)` for equal offsets, the 3×3 system
  (rows n₁, n₂, e; rhs d₁, d₂, 0) for unequal, and the 3-plane corner solve at
  chart-corner vertices (conditioning ∝ 1/|det|: the dangerous vertex is three
  near-coplanar strips, i.e. the curved wall — which the registry merges, so it
  never reaches the solve). Never a normalised bisector (law `f713599ea11d`).
  Mitre limit 4 → bevel quad (two vertices) past it.
- The heights d₁, d₂ at the ring come from the **junction rule** (item 1.8):
  dominant owner → one value both sides; steps → each side's own, joined by the
  planar step face on the bisector (a real face in the mesh, watertight by
  construction).
- Concave junctions (wall–floor): the same solve; under union nothing else — no
  pin, no free row (the base-junction bug, bc79e39d, cannot exist: the floor's
  ring and the wall's ring are the same vertices).
- Invariant: every ring vertex lies on both offset planes to 1e-5 u;
  `refrender_diff` at the junction band (crease map, `creasemap_<pose>.png`)
  reads the same |Δd| the census predicts for that junction. Instrument:
  `[V4-RINGS]` + the crease-dh map.

### (f) Free edges
- Classified in (a); the skirt is a quad strip from the displaced edge down to
  the slab back (amplitude), emitted only for use-count-1 edges that are not
  silhouettes of a lid. Invariant: skirt count == free-edge count (4 + the listed
  silhouettes); the doorway pose t=5963/5928 shows relief to the jamb
  (his 2026-08-11 ask, the old `free_edge` flag's job) — `refrender_diff` at the
  doorway p90 < 0.08 u (today 1.22).

### (g) Shading normals
- `n = normalize(N − (h_u/s_u)·T − (h_v/s_v)·B)` per chart, exact; gradient taken
  at the same mip as the height (or geometry and shading disagree, survey §C);
  split normals at creases (> 30°); the smooth base normal only within a merged
  chart. `Compute_Vertex_Normals` is **not** called on the baked mesh.
- Invariant: normal-angle vs the reference p50 < 5° on stone pixels (today
  4.7–21°); zero pixels where the v4 normal is > 90° from the face normal
  (the inversions of 41ff72ed).

### (h) Output
- One TriMesh per baked mesh; UVs per face corner; materials unchanged; the
  `::mirUV` clone split runs after, as today.
- Invariant: watertight (use-count 2 except the listed boundaries), zero
  T-junctions, sliver census (min-angle p10 > 2°, none < 1°), face count ≤ the
  old bake's, `[V4-OUT]` prints all four and `tools/v4_out_census.py` reads
  `displace_faces.txt`.

## 3. The seven failure modes → the stage that prevents each

| # | failure (old bake) | v4 stage that makes it impossible | ledger | regression detector |
|---|---|---|---|---|
| 1 | displacement direction = polluted smoothed normal (three doors: mitre bisector dd798c31, groove target 41ff72ed/16aac2ee, ride sign a0c46c40) | (e): direction is the chart plane normal; no smoothed field exists in v4 | law `420fcd4626b8`; corpus states 2–5 (`71676e39b432`…`f982d744206c`) | `[STONE-RIDEPROV]`-style provenance is unnecessary; the detector is `refrender_diff` normal-angle > 90° count (must be 0) |
| 2 | reflex mitre with a wrong-sign partner | (e): offset-plane solve, sign-free by construction; mitre limit 4 | law `f713599ea11d`; refuted lever reflex weld (state 7, `4c091eb140bc`) | ring-vertex residual to both offset planes (1e-5 u) |
| 3 | T-junctions and near-coincident twins (edgeVert float-bit key, profile twins, tsplit after displacement) | (c): R1/R2/R3, exact `i/n` borders, one vertex per border sample; watertight **before** displacement | law `7453a529070e`; `0c2cd2a858e6`, `c165bff2f03a`, `20be1fac463e` | undisplaced-arm tear battery = 0; use-count census; T-vertex census |
| 4 | edges misclassified free (floor face excluded as "own face"; wall-end columns) | (a)+(f): material is an attribute; free = use-count 1 only; skirts after classification | `10994f6ef014`; bc79e39d (sibling_abut) | free-edge count == 4 + listed silhouettes; base-row tear class = 0 |
| 5 | grooves over-carved / plateaus recessed (lattice nodes on the blur's shoulders) | (c)+(d): nodes ≥ 4 texels inside blocks, max/min pyramids | `d8e1d26bfc3e` (−0.036), `40ac61208dfa` | `nspace_relief.py` plateau bias within ±0.005 |
| 6 | per-block pillow shading (blend toward the smoothed normal at every grout band) | (g): exact gradient normal per chart, no blend | `864d9658ae3a` (crease gate refuted on the wrong field); state 4 (`39daeb3a13c8`) | normal-angle p50/p90 vs reference; `bulge_detect.py` GBI on the pier front (must read the bare floor) |
| 7 | curved strips vs true creases under one rule | (b): chart membership (normal budget) and crease threshold are two predicates | `60e3e63bed65` (cot-sized smooth seam replaced the curved wall) | curved-wall pose t=2845 dz p90 < 0.06 (today 0.058 — the one pose the old bake gets right must not regress) |

Not a failure mode but a debt v4 retires with the same stage: the corner-column
fan (73 vs 11 faces/u², 154 sub-1° slivers) — stage (c)'s ≤1-level rule; detector
= the sliver census in (h).

## 4. Validation plan

1. **The reference renderer is the yardstick at every round.** `tools/refrender_battery.sh`
   (lean mode, self-cleaning, aborts under 5 GiB free) at the corpus poses and
   the review list: per-pixel signed dz, normal angle, the Laplacian-weighted
   crack map, front-face-cull views (Lindstrom & Turk) — `docs/img/refrender/`
   conventions, numbers into `refrender_numbers.json` and the ledger as
   `greets.displace.v4.dz` measurements per pose, contract `world_units_range`,
   scope `{arm: v4, pose}`.
2. **Tear battery** (54 poses + H6194): 0 holes is the bar, per pose, not on
   average; `tools/tear_detect.py` HOLE class only.
3. **Polygon census** of `displace_faces.txt`: total faces, per-chart density,
   min-angle distribution, corner-column density ratio — `tools/v4_out_census.py`.
4. **Gates**: 14 pins + render_gate 4/4 + warm_gate --full 7/7, byte-identical
   with `--greets_displace_v4` OFF (the flag is the only entry point); the
   undisplaced v4 arm byte-identical to the bare wall.
5. **Perf**: bake wall-clock from the `[V4-*]` banners (≤ 500 ms); greets tick at
   t=5743/5965, min-of-11 interleaved, both arms on the same binary, judged
   against the ±1.5–2 % placement floor (`perf.floor`); face count vs the old
   bake.
6. **His eye**: the fly-through command line with the flag, and the
   before/after pairs at the corpus poses with full paths — the only acceptance
   for look. Every render carries its provenance sidecar (b12f6b59).
7. **Locality while both bakes coexist**: `--greets_displace_v4` changes stone
   pixels only; a diff mask against the old default must be empty off the
   stone materials.

## 5. Migration

- `--greets_displace_v4` (BOOL, default OFF) selects the bake; the old
  `DisplaceStoneSubdiv` and its flag family (45 `greets_displace_*` flags on
  fog-wt, ~10 more on rev-dispfix: tsplit, border_weld, mitre_notch_taper,
  sign_gate, shoulder_plateau…) stay untouched and shipped until v4 is the
  default **by his fly-through** (a `decision` record with his quote, like
  `45fa253bbed3`). Then the old bake and its flags are removed in one commit with
  the byte-identity of the v4 default proven before and after the removal.
- rev-dispfix (mitre notch taper, 5 072 → 478 at H6194) merges on its own merit
  as the interim shipped path; nothing in v4 depends on it.
- Instruments that survive (they measure against ground truth, not against the
  old bake's internals): `tools/tear_detect.py` + battery, `tools/refrender_diff.py`
  + battery + `refrender_creasemap.py`, `tools/tear_cover.py`, `tools/nspace_relief.py`,
  `tools/refdiff_detect.py` (the plane+heightfield reference — now second to
  the renderer, kept for its marks-region gate), `tools/bulge_detect.py` (GBI,
  shading only), `tools/mark_faces.py`. Retired with the old bake: every
  `[STONE-*]` census that names its internal classes (FREEV, EDGEVERT, RIDEPROV,
  JSQ, PLANEFRONT, BWELD, EVM), `tools/tear_verts.py` (joins to old-bake verts),
  `tools/bridging_verify.py`, `tools/tear_pairs.py`.
- `docs/BULGE_CORPUS.md` gains v4's states as they are judged; the `look`
  verdicts on the old arms stay as history.

## 6. Implementation phases (each a gated round with its own measurement)

| phase | builds | ends when (instrument) | risk |
|---|---|---|---|
| P1 registry + stitch | (a)+(b): half-edge over the authored soup, chart registry, junction table with census class | `[V4-STITCH]`/`[V4-CHARTS]`: 0 non-manifold edges, free edges listed (4 + silhouettes), ~24 planes + curved chart, every junction classified; no pixels change (nothing rendered yet) | low; the `::mirUV` ordering and the hidden-track meshes (§REF "five things" 1–2) are the traps |
| P2 lattice, undisplaced | (c) with amp forced 0 | tear battery **0** on the undisplaced arm; pins byte-identical to bare; T-vertex census 0; sliver census; face count | medium: CDT with breaklines inside a restricted quadtree is the most code; R3 exactness on borders |
| P3 plateau displacement | (d)+(e) interiors only, rings pinned at 0 | `nspace_relief` plateau bias within ±0.005 at all planes; `refrender_diff` dz p50 at block interiors ≤ 2× bare | low |
| P4 corners | (e) rings by the offset-plane solve, junction rule per census class | crease-dh map matches the census per junction; H6194/cam A/corner6097 dz p90 < 0.08; tear battery 0 at the wall-end poses (S6120, P6133, S6150, H6194) | **high** — this is the month's defect; the reference has both arms rendered for his eye |
| P5 free edges | (f) skirts | doorway t=5963/5928 dz p90 < 0.08; skirt count == free count | low |
| P6 normals | (g) | normal-angle p50 < 5°, p90 < 15°; zero inversions; GBI at the bare floor | low |
| P7 density + perf | tune `cpb`, groove refinement level, chart budget | init ≤ 500 ms, faces ≤ old bake, tick within the floor, sliver census clean | medium: perf vs relief trade is his call per §4.5 |

Each phase files its measurements as records under `greets.displace.v4.*` and
ends with the coordinator's `check` before the next phase's proposal is built.
P4 cannot start before his rulings in §7 unless the assumptions stand.

## 7. Open for Gil-Ad

1. **Membership** (`greets.displace.v4.ruling.membership`): union (assumed) vs the
   literal partition. Evidence: `e43c035dbedf`; both arms in
   `docs/img/refrender/camA_variants.png` panels 1–2.
2. **Corner rule** (`greets.displace.v4.ruling.corner_rule`): the census rule
   (dominant on the 73 % phase-shifted, steps on the 21 % unrelated — assumed) vs
   uniform steps (his 2026-08-29 ruling) vs uniform dominant. Evidence:
   `83aa7c0b6c4c`, `docs/img/refrender/junction_<pose>.png`.
3. **The 21 %** (`greets.displace.v4.ruling.unrelated_charts`): accept the
   castellation there, or re-author the UVs at the named junctions so the two
   sides agree in fact. Assumed: accept for v4's first landing; the census's
   junction list is the work order if he wants them fixed.
4. Not his but undecided: the crease threshold (30°, artist convention, no
   measurement); `cpb` and the groove refinement level (P7, perf vs relief);
   whether the lid faces (pier tops) take the junction rule or are always
   dominant-to-the-wall.
