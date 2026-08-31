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

---

# §P1 — the round that built (a)+(b), and what it measured

Built on `rev-v4` off `fog-wt` **99bf16b5** (the design was written against
fcca1ed7; fog-wt has since advanced). Code: `DEMO/V4Bake.{h,cpp}`, flags
`--greets_displace_v4` / `--v4_census` / `--v4_chart_budget_deg`, readers
`tools/v4_census.py` and `tools/v4_p1_gate.sh`. Commits 84f5a585, de59ba3a,
02d967be. Census only: the pass holds every scene pointer `const`, prints two
stderr blocks and returns; the old bake then runs unchanged.

Reproduce the whole round in one command:

```sh
/Users/gil-ad/work/rev-v4/tools/v4_p1_gate.sh              # table + the eight P1 checks
/Users/gil-ad/work/rev-v4/tools/v4_p1_gate.sh --json       # what the ledger rechecks parse
/Users/gil-ad/work/rev-v4/tools/v4_p1_gate.sh --log        # the raw [V4-*] block
```

## The gate — 8/8 PASS

| check | result |
|---|---|
| 0 non-manifold edges | `use3plus=0` |
| free edges, classified against the whole soup | `free=0`, `shared_soup=130`, `coincident=0` of `use1=130` |
| 0 ε-fallback welds | `eps_merges=0` at ε = 4.937e-4 u = 1e-3 × the shortest authored edge (0.49374 u) |
| no null twin | `null_twin=0`, 130 boundary half-edges carrying a null FACE |
| every stone face in exactly one chart | 226 of 226, `unassigned=0` |
| chart fits its budget | `over_budget_charts=0`, max in-chart deviation **0.0000°** |
| winding consistent on every shared edge | `orient_flip=0`, `convex_disagree=0` |
| census wall-clock ≤ 50 ms | 9.5–10.1 ms internal, **+12.8 ms** on the displace block (min-of-5), 0 with the flags off |

Byte identity, greets cam A t=5965 on the judging flags at 1920×1080 —
**`d92cb6f5eb19da4301588ae83af6a56e`** three ways: the pre-change binary built
at 99bf16b5, the post-change binary with the flags OFF, and the post-change
binary with them ON.

## The scene, as measured rather than as assumed

The bake set is far smaller than the design's prose implies, and everything
below follows from that one number.

| | |
|---|---|
| meshes carrying `rooms`/`floor` at the bake point | **1** (`Piramid.lwo`, 5532 faces total) |
| authored stone triangles | **226** (196 `rooms`, 30 `floor`); 0 degenerate, 0 skipped by the old bake's height-map guard |
| face corners → vertices | 678 → **155**, by BITWISE key alone (523 exact merges, 0 ULP, 0 ε) |
| edges | 404 = 130 use-1 + 274 use-2 + **0** non-manifold |
| edge attributes | 157 coplanar, 105 crease (≥30°, 629.4 u), 169 smooth, 33 material seams (333.5 u), 21 convex / 84 concave creases |
| whole-scene soup at the bake point | 8880 faces over 7 meshes, 26 materials |
| charts at 10° | **73**, max in-chart normal deviation 0.0000° |
| distinct planes (the reference's own 0.5°/5 mm clustering) | **52** |
| chart-pair junctions | **104**, 676.5 u (21 convex, 84 concave, 12 smooth edges) |
| plane-pair junctions | **142**, 1904.18 u |

### 1. The scene has **0** free edges at the bake set, not 4 — and the "4" is real, just elsewhere

§1.7 and §2a expect "the scene's 4 free edges plus doorway/lid silhouettes".
Measured: 130 edges are used by exactly one *stone* face, and **all 130 have a
foreign soup face at distance 0** under the reference renderer's own rule (a
soup face sharing the edge by position, else any soup face within 0.05 u of
three interior points). By abutting material: `rooms` 60, `siling` 50,
`floor` 14, `teleporter` 6. Zero are free; zero are the tolerance-only
"coincident" class.

That the count is *not* zero for `rooms`/`floor` is itself the finding: 74 of
the 130 abut another **stone** face that the exact stitch cannot see, because
the two sides are not co-segmented — T-junction abutments, of which the
wall-base-on-floor concave junction is the archetype (§2e already predicts that
"the floor's ring and the wall's ring are the same vertices" is a property v4
must *create*; in the authored mesh they are not).

The design's 4 is a property of the **render-time** scene: the reference reports
`free 4` over 892 stone faces with mirrors on, and `free 0` over 226 with
`--no-greets_mirror` — the same set and the same answer as this census.
Consequence for §2f/P5: **skirt count == 0 at the bake set**, and the doorway
relief P5 owes his 2026-08-11 ask has to come from the 130 classified
abutments, not from a free-edge rule. Ledger `418c3bf6c9d7`.

### 2. The crease census's 453 junctions reconcile exactly — they are the mirror clones

§1.8 and §2b hang the corner rule on "453 junctions, 7375 u" (`83aa7c0b6c4c`).
Measured on one binary, both ways:

| arm | authored stone faces | plane-pair junctions | crease length |
|---|---|---|---|
| default (mirrors on) | 892 | **453** | 7375.2 u |
| `--no-greets_mirror` | 226 | **142** | 1904.2 u |
| v4 bake-time census (226 faces, same clustering) | 226 | **142** | 1904.1782 u |

The bake-time census and the reference agree on the count exactly and on the
length to 1e-4 u — two independent implementations, one at init on model-space
authored faces, one at render time on transformed ones. `83aa7c0b6c4c` is
**not** wrong: re-run at its own recipe (`--greets_displace_ref_crease_scan=4`)
it reproduces 453 / 7375.2 u / 1085232 samples / 21.2 % unrelated bit for bit.
It simply counts a population 4× the bake's, because `GreetsMirror` clones the
room geometry for the teleporter and the three `P_TEXT` mirrors **after** the
bake — the same ordering as the `::mirUV` split, and the same reason v4 must
bake the 226 and let the clone build copy the result. The ratio is not exactly
4 (453 vs 4×142) because the clones are mirror-bbox-clipped partial copies and
the plane identity is a greedy first-match cluster. **The junction population
v4's corner rule serves is 142, not 453.** Ledger `9a42678e68cf`.

### 3. There is no curved-wall chart to merge — the scene is strictly piecewise-planar

§2b expects "~24 dominant planes plus the curved-wall chart", the curved wall's
narrow strips merging under the normal budget. Measured: **every** chart has a
max in-chart normal deviation of **0.0000°** — every merge this scene admits is
between exactly coplanar faces. The budget sweep says why:

```
budget°:charts   1:73  5:73  10:73  15:73  20:71  25:69  29:62  30:61  45:47
```

The count does not move at all from 1° to 15°: **the shallowest stone dihedral
in the bake set is 17.6°**, and only 12 shared edges sit below the 30° crease
threshold at all (17.6°, 20.5°, 25.5°, 26.0°, 27.3°, 27.7°, 29.1°, each a
single edge, at (±3.67, ·, 0.45), (±3.07, ·, 4.24), (9.87, 2.47, −49.37),
(5.52, 2.47, −51.69)). Whatever the curved wall of trap `60e3e63bed65` is, it is
not in the `rooms`/`floor` faces the bake targets.

"~24 dominant planes" reconciles as an **area** claim, and on that reading it is
close: of 6455.4 u² of stone, 5 charts carry 50 %, **27 carry 90 %**, 54 carry
99 %. The registry has 73 charts over 52 planes (8 planes host more than one
chart, one hosts 6) because a chart is *connected* and a plane is not.

Two consequences for later phases: with no sub-crease strips, the chart budget
is **not** a live dial in this scene (anything in 1–15° gives the same registry),
and the §2e warning about "three near-coplanar strips" conditioning the 3-plane
corner solve does not arise here.

### 4. The stitch needs no tolerance whatsoever

Survey §E's "authored coordinates are bitwise equal at shared corners" holds
exactly: 678 corners → 155 vertices on the bitwise key alone, with the
single-ULP pass and the ε fallback each merging **0**. The ε fallback is built
and instrumented (it prints every merge, ε relative to the shortest authored
edge) but never fires. Ledger `bcd934f525da`.

Also measured, and worth having on file: all 274 shared edges are wound
consistently (0 flips) and their convex/concave sense agrees computed from
either side (0 disagreements), so §REF "five things" #4's winding hazard is
absent from this mesh — the census still orients every normal by the engine's
`F.N` rather than trusting that.

## Deviations from the design, stated

1. **Chart growth is a dihedral union-find, not a Lloyd-iterated VSA.** A face
   joins the chart when the dihedral across the shared edge is ≤ the budget.
   Two reasons: it is what merges a curved wall (each strip differs from its
   *neighbour* by a few degrees while the chart as a whole may span far more
   than the budget), and it is order-independent by construction — the charts
   are exactly the connected components of the sub-budget edge graph, so the
   registry is reproducible without a seed order. The L^{2,1} proxy the survey
   names is still computed and **reported** per chart (area-weighted normal plus
   the max deviation from it), so the number that would refute the choice is on
   the census. On this scene the two agree trivially: max deviation 0.0000°.
2. **The design's `envDoms` block at `MeshOps.cpp` ~2082 does not exist** on
   fog-wt 99bf16b5 (that line is inside `DisplaceStoneSubdiv`'s seam grid), so
   the "reused read-only" precedent for the plane registry was written fresh.
3. **The bake set is model space, not world space.** `Animate_Objects` has not
   run at the bake point, so `TriMesh::RotMat` is `{0}` and `IPos` `{0,0,0}` —
   the reference's `WorldPos()` cannot be reused here. The census reads the
   splines' keys directly (no `Spline_Calc_*` call, which would mutate
   `CurKey`) and reports the placement: `Piramid.lwo` carries 2-key Pos/Scale/
   Rotate splines whose first key is identity, so it is flagged `animated=1
   identity=0` and a warning is printed. With exactly **one** stone mesh this is
   moot for topology — every length, dihedral and adjacency in the census is
   invariant under the mesh's rigid placement — but the world coordinates in the
   `USE1` / `JUNC` / `CHART` rows are model-space, and **P2 must carry the
   placement explicitly** before it emits anything positional.

## What P2 inherits

* A stitched, edge-manifold, consistently wound half-edge over 226 faces /
  155 vertices / 404 edges, with 0 tolerance anywhere in it.
* 73 charts, 52 planes, 104 chart-pair and 142 plane-pair junctions, every
  junction carrying φ, class and length.
* 130 use-count-1 edges already classified against the whole soup, with the
  abutting material named per edge — the input P5's skirt rule needs, and the
  input §2e's concave wall/floor ring needs.
* Two open corrections to this document's premises: **0 free edges** at the bake
  set (§1.7, §2a, §2f, P5) and **142 junctions**, not 453 (§1.8, §2b, P4).

---

# §P2 — the round that built (c), and what it measured

Built on `rev-v4` at P1's tip **7c621be8**. Code: `DEMO/V4Bake.cpp`
(`RunP2Bake`), `DEMO/V4Border.cpp` (the one shared-border position function,
`-ffp-contract=off`), the groove-grid finding extracted out of the old bake into
`MeshOps_FindStoneGrooveGrid`, flags `--v4_cpb` / `--v4_groove_refine` /
`--v4_abut_split` / `--v4_max_border_seg` / `--v4_flat`. Commits c987060e,
cf0a6db7, b9dae19d. **`--greets_displace_v4` now SELECTS THE BAKE**: with it on
the lattice replaces the authored stone faces and the old bake does not run on
them; with it off nothing changed at all.

Reproduce:

```sh
/Users/gil-ad/work/rev-v4/tools/v4_p2_pose.sh /tmp/v4p2 5965 \
  "22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \
  --greets_displace_v4 --v4_census
python3 /Users/gil-ad/work/rev-v4/tools/v4_census.py --p2gate /tmp/v4p2/log.txt
/Users/gil-ad/work/rev-v4/tools/v4_p2_gate.sh                    # the two-tier byte gate
/Users/gil-ad/work/rev-v4/tools/v4_p2_gate.sh --test old0        # the same gate on the SHIPPED bake at amp=0
/Users/gil-ad/work/rev-v4/tools/v4_tear_battery.sh /tmp/v4tear   # 54 poses
```

## What the lattice is, as built

Per authored stone triangle: the boundary is its three corners plus the
**edge-owned** samples of its three edges; the interior is the height map's own
grid, clipped to the triangle and inserted as Steiner points; the triangulation
is a Bowyer–Watson Delaunay in the face's own plane, which is exact here because
the domain is a TRIANGLE — convex, so every consecutive pair of boundary points
is a hull edge and therefore a Delaunay edge, and the edge-owned boundary
survives untouched. 0 fallbacks fired over the 226 faces.

**Deviation from §2c, stated: the lattice is defined per FACE, not per chart.**
The grid lines live in the height map's own texel space and are therefore global,
so every face of a chart sees the same breaklines without any chart-local frame;
each face converts a map coordinate to a position through its **own barycentric
solve**, which keeps every emitted point an exact convex combination of the
authored corners — i.e. exactly on the authored plane, which is what "amp = 0 ⇒
the same planes" needs. It also removes a whole failure mode (a chart whose faces
do not share one affine UV map) without a special case. Charts are still built
and still reported; P3+ needs them for the height rule, not the lattice.

| | |
|---|---|
| breaklines | `MeshOps_FindStoneGrooveGrid`, extracted VERBATIM out of `DisplaceStoneSubdiv` so both bakes run the identical float expressions (the flag-OFF byte gate is what proves the extraction moved nothing). v4 keeps the FINDING and drops `StoneLineRep`, the per-line rep heights. `rooms`: 4 h-grooves / 4 bands / 12 template rows at pitch 64×64 texels of a 256×256 mip-2 map. `floor`: 12 / 12 / 36 at pitch 43×43. |
| interior density | `--v4_cpb` (default 1.0, the old bake's own lever and its own default), target cell = block pitch / cpb texels; world cell 1.50–2.50 u across the scene |
| groove bands | `--v4_groove_refine`, **default 0, not the design's 1** — see the measurement below |
| plateau nodes | min distance to a mortar RUN edge **5.000 level-0 texels** (the shoulder pad is 1.25 mip-2 texels = 5 level-0, so the rule holds by construction; §2c asks ≥ 4) |
| restricted quadtree | rows are full lines through the face and a row boundary carries the UNION of the column sets above and below it, so a density change makes no hanging node at all; **0** level-jump violations |
| borders (R1/R2/R3) | sample count from the edge's two ENDPOINTS alone (each vertex carries the finest world cell of any stone face touching it, so both sides read one number); parameters exact `i/n` in integer arithmetic in `V4Border.cpp` at `-ffp-contract=off`; one vertex per sample, indexed from both faces; endpoints ordered by world position (the stitch's vertex ids ARE that order). 1494 interior samples over 404 edges, 1..27 per edge, **max deviation from the exact authored line 1.42e-14 u** |

## The output census — §2h, measured

| invariant | target | measured |
|---|---|---|
| the two topology builds agree | — | P2's independent stitch reads **226 faces / 155 verts / 678 corners / 404 edges / 130 use-1 / 274 use-2 / 0 non-manifold / 73 charts**, digit for digit what P1's census reads |
| use-count 2 everywhere except the listed abutments | yes | 91 224 output edges: use-2 **90 594**, use-1 **630** = exactly the 130 authored abutments cut into their own segments, use-3+ **0** |
| ZERO T-vertices | 0 | **278** — and every one of them is on an authored abutment LINE: by class, corner 36 / abutment-sample 242 / **shared-border sample 0** / **interior node 0**. R1/R2/R3 produce no T-vertex at all; the 278 are the scene's pre-existing T-junction abutments (the flat control reads **40** with no lattice anywhere), which §2e assigns to P4 |
| sliver census p10 > 2°, none < 1° | p10 > 2, 0 | p10 **2.2551°**, p50 23.33°, min **0.9742°**, **8** faces of 60 606 under 1° (0.013 %), 3 854 under 2° of which 972 lie inside a mortar band — the kind Dyn–Levin–Rippa call legitimate (survey §D) |
| face count vs the old bake | ≤ ~85–90k scene faces | mesh **65 912** faces / 34 299 verts, of which **60 606** are stone over 6 455.43 u² = 9.39 faces/u². The old bake on the same mesh: **95 931** faces / 54 893 verts (90 625 stone). v4 is **67 %** of its faces and **63 %** of its vertices |
| corner-column density | ≤ 2× the wall's | per-chart 1.366 … 12.141 faces/u², ratio **8.9** — better than the old bake's 73-vs-11 (6.6×) only if read per chart rather than per column; the honest statement is that the fan is gone (0 profile densification) but the density ratio is still driven by how finely a wall is UV-tiled, which is the map-relative rule working as designed |
| bake wall-clock | ≤ 500 ms | **49.8 ms** min-of-5 for the whole displace block with the lattice (old bake **476.5 ms**; the v4-flat control 20.8 ms is the floor the residual-height/cone-map rebake costs) |

## The gate — and the invariant §2c got wrong

§6 P2 asks the undisplaced arm to render **byte-identical to the bare wall**.
The control for that has to be the same pipeline WITHOUT the lattice, not
`--no-greets_displace` (which also swaps the POM input map and two companion
flags), so `--v4_flat` was built: the v4 bake runs its whole pipeline — topology,
chart registry, mesh rebuild, parent-plane stamping — and emits the 226 AUTHORED
triangles. Everything downstream is then bit-for-bit the same in both arms.

**Tier 1 fails, and it fails for a reason that has nothing to do with v4.**
At cam A + the 18 review poses, 0 of 19 are byte-identical. The mechanism is
`FDS/FRUSTRUM/FRUSTRUM.CPP:945`: the albedo mip is chosen **per clipped
polygon** (`pixArea < MinSize` takes one mip for the whole polygon; above it the
mipmap-via-subdivision path splits it), so ANY change to the stone's tessellation
moves which mip a pixel samples — **265 108 pixels change mip level at cam A**,
which is what the 48.9 % colour difference is.

Put the SHIPPED bake at amplitude 0 through the identical gate and it does the
same thing, slightly worse:

| test arm vs the `--v4_flat` control, cam A | z16 ≠ (of which 1 quantum) | z16 outliers | raster-vs-empty | matID flips (non-stone) | colour ≠ | max Δ |
|---|---|---|---|---|---|---|
| **v4 undisplaced lattice** | 2 828 (2 826) | 2 | 1 | 5 (3) | 48.91 % | 197 |
| **old bake at `--greets_displace_amp=0`** | 3 149 (3 145) | 4 | 3 | 6 (4) | 50.73 % | 197 |

over the 19 poses: v4 tier-2 pass on 3, fail on 16; the old bake at amp=0 tier-2
pass on 2, fail on 17. **The design's §2c invariant is not achievable by any
retessellation in this engine and should be struck**; what it should say is that
the undisplaced arm must be no further from the flat wall than the shipped bake
is, which v4 satisfies on every column of that table.

Tear battery, 54 poses, control-referenced (a pixel the control rasterises and
the arm does not, with amp = 0 so both cover the same planes):

| arm | hole px | poses with a hole |
|---|---|---|
| **v4 undisplaced lattice** | **15** | 9 of 54 |
| old bake at amp=0 | 25 | 13 of 54 |

Not the 0 the phase asks for. They are single pixels (1–4 per affected pose) at
silhouettes, they are not a mesh gap (0 non-manifold edges, 0 lattice-created
T-vertices), and the shipped bake's own retessellation produces two thirds more
of them.

**The old default is untouched**: greets cam A t=5965 on the judging flags is
`d92cb6f5eb19da4301588ae83af6a56e` at this tip with the flag off — the same
value P1 recorded three ways; greets t=1588 pin `570a7b443f768393dc6647044a9e67b3`;
`render_gate.sh` 4/4; `warm_gate.sh --full` 7/7.

## Two dials the round measured rather than assumed

1. **`--v4_groove_refine` defaults to 0, not the design's 1.** A mortar band is
   only 2.5 texels tall at the bake mip; splitting it again makes 48:1 cells.
   Measured: at 1, 202 108 stone faces (2.2× the old bake) and min-angle p10
   **1.20°** (§2h wants > 2); at 0, 60 606 faces and p10 **2.26°**. The dial is
   exposed and P7 (§6) owns the tuning.
2. **`--v4_abut_split` defaults to ON.** The 130 use-count-1 edges are
   abutments, not boundaries, and 74 of them are already T-junctions in the
   authored mesh. Measured: ON → 8 faces under 1° (min 0.974) and 278
   T-vertices, every sample within 1.4e-14 u of the exact authored line; OFF →
   40 T-vertices (the authored count, the lattice adds none) but **990** faces
   under 1° (min **0.014°**) because interior nodes fan to an unsplit edge up to
   12 u long. ON ships because a 0.014° sliver is a rasterizer hazard while a
   coincident-but-unshared point on a line that is already a T-junction is not.
   The real fix — the floor ring and the wall ring being the same vertices — is
   §2e, phase P4.

## What P3 inherits

* A watertight undisplaced lattice: 60 606 stone triangles over the 226 authored
  ones, use-count 2 except the 630 abutment sub-edges, 0 non-manifold, 0
  T-vertices created by any shared border or interior node, 0 triangulation
  fallbacks, bake 49.8 ms.
* Border samples that are exact `i/n` convex combinations of the authored
  endpoints, shared by index, computed in a `-ffp-contract=off` TU — the R1/R2/R3
  machinery P3's displacement will ride.
* Two corrections to this document: **§2c's byte-identity invariant is not
  achievable in this engine** (the per-polygon mip choice), and **§2c's "groove
  bands one level finer" costs the §2h sliver invariant and 2.2× the face budget
  on this scene**.
* One P4 item, already named by §2e: the 130 abutments are coincident lines, not
  shared rings. Until they are, 278 points on them are T-vertices — harmless at
  amp = 0, and exactly what "P3+ cannot open the base junction" depends on P3
  pinning displacement along them.

---

# §P3 — the round that built (d)+(e) for interiors, and the gate it did not reach

Built on `rev-v4` at P2's tip **b8b51422**. Code: `DEMO/V4Bake.cpp` (the height
field, the pyramids, `ReliefClassAt`, the displacement in `RunP2Bake`,
`AccumRelief`), flags `--v4_amp` / `--v4_pyramid` / `--v4_pyr_radius_tex` /
`--v4_relief_census` / `--v4_band_union`, readers `tools/v4_census.py --p3
--p3gate`, `tools/v4_p3_num.sh`, `tools/v4_p3_ref.sh` and the `--flat-deg`
block-interior mask on `tools/refrender_diff.py`. Commits 7f21c96d, b518d7a2,
f53dc4d6, 58b0c02e.

**The headline: P3 built exactly what §6 P3 proposed and reached neither end
condition, and the reason is not the height rule.** The same height rule, with
one node added inside each block, passes the geometric condition at every corpus
pose and beats the flat wall — which nothing in this campaign had done before.
What blocks it is the lattice, which is §2c's stage and §6 P7's dial.

Reproduce:

```sh
/Users/gil-ad/work/rev-v4/tools/v4_p3_num.sh plateau_emr_p50          # the census number
/Users/gil-ad/work/rev-v4/tools/v4_p2_pose.sh /tmp/v4p3 5965 \
  "22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" \
  --greets_displace_v4 --v4_census
python3 /Users/gil-ad/work/rev-v4/tools/v4_census.py --p3gate /tmp/v4p3/log.txt
/Users/gil-ad/work/rev-v4/tools/v4_p3_ref.sh /tmp/v4p3ref                # dz, 4 poses
/Users/gil-ad/work/rev-v4/tools/v4_tear_battery.sh /tmp/v4tear           # 54 poses
```

## What P3 does

Every **interior** lattice node moves along its **chart plane normal** by
`d(u,v) = amp·(h(u,v) − mipMean)`. Every vertex on an authored edge — corner,
shared-border sample, abutment sample — stays **pinned at 0**: §2e's
offset-plane solve is P4, so nothing two faces share moves, and P2's
watertightness and every silhouette carry forward untouched (measured: 1 649
pinned vertices, 28 946 moved, `border_sample` and `interior` T-vertices still 0).

| | |
|---|---|
| direction | the chart's area-weighted proxy normal; max angle to any member face **1e-06°**, so §2b's "material-blind chart" and the face normal are the same thing on this scene, as P1 measured |
| height field | the mip is unswizzled once per material into a row-major copy with this file's **own** copy of the block-tiled address and its own bilinear (texel centres, toroidal wrap) — a second implementation of the convention `MeshOps.cpp` and `DeferredDisplaceRef.cpp` each carry, which is what makes the census's `r` ground truth rather than self-agreement. `rooms` mipMean 0.547138, h ∈ [0, 0.6627], d ∈ [−0.1641, +0.0347]; `floor` 0.340478, [0.1725, 0.4588], [−0.0504, +0.0355] |
| pyramids | separable max/min dilation over a `--v4_pyr_radius_tex` (default 1.25 = `kStonePadTex`) window with toroidal wrap. A max over a window **is** the max-pyramid at the level that covers it, without quantising a 1.25-texel shoulder to a power of two |
| classes | from the mortar grid, never a threshold on the field: inside a run = groove (min-pyramid), inside its shoulder pad = bevel (bilinear), outside = plateau (max-pyramid) |
| bake cost | displace block **55.8 ms** min-of-5 (`--v4_amp=0` 52.2, `--v4_flat` 23.3) against a 500 ms budget; the [V4-RELIEF] census is timed separately and runs only under `--v4_census` |

### The convention at the outer pad lines, which had to be measured

The first implementation called both pad lines of a mortar run "bevel". It
produced **plateau = 0 nodes in the whole scene** and reproduced the −0.036 u
recession of `d8e1d26bfc3e` exactly. At `--v4_cpb=1` the target cell **is** the
block pitch, so `ceil(pitch/target) = 1` and *every* lattice node in this scene
sits on a breakline — there is no node in a block interior for a "plateau node"
rule to apply to. The outer pad line (`lo−pad`, `hi+pad`) is the **top of the
shoulder** and is therefore a plateau node reading the max-pyramid; the inner
pad line is the bottom and is a groove node reading the min; the ramp between
them is the bevel. That fixed the classification (8 638 plateau / 20 308 groove
nodes) and moved the scene bias −0.0720 → −0.0556, which is where the rule ran
out of nodes to work with.

## The gate — 6 PASS, 2 FAIL

| row | target | measured | |
|---|---|---|---|
| OFF arm byte-identical | every pin | cam A `d92cb6f5eb19da4301588ae83af6a56e`, greets t=1588 `570a7b443f768393dc6647044a9e67b3`, fountain t=2500 `8db68ccb59416e9a44037e9f387b7bd9`, `render_gate.sh` **4/4** | **PASS** |
| v4 arm deterministic | same md5 24/24 | `7c259253ca3b540de5c51e60417765f3`, 24 of 24, 0 flips | **PASS** |
| tear battery | ≤ 15 hole px | **14** over 8 of 54 poses (P2: 15 over 9) | **PASS** |
| sliver census | no regression past P2 | min-angle p10 **2.4066°** (P2 2.2551), p50 20.84, the same **8** faces under 1° of 60 606, 2 913 under 2° (P2 3 854) | **PASS** |
| face count | ≤ the old bake's | **60 606** stone / 65 912 mesh, unchanged — P3 moves nodes and adds none; the old bake is 90 625 / 95 931 | **PASS** |
| bake wall-clock | ≤ 500 ms | **55.8 ms** min-of-5 | **PASS** |
| plateau bias | ±0.005 u on every plane | e−r p50 **−0.0556** u scene-wide, **−0.1065** in the core band, **33 of 33** planes outside, worst −0.1189 | **FAIL** |
| dz at block interiors | ≤ 2× the bare floor | **2.91–4.84×** at 4 of 4 corpus poses | **FAIL** |

Watertightness is carried forward intact: 91 224 output edges, use-2 90 594,
use-1 630 = exactly the 130 authored abutments cut into their own segments,
use-3+ **0**; 278 T-vertices, `border_sample` 0 and `interior` 0, i.e. every one
of them still on an authored abutment line, which is §2e's and P4's.

## Why it fails, localized

Not the height rule and not the pinned rings — the **core** band (samples more
than one target cell from an authored edge, i.e. with P4's rings excluded) reads
**worse** than the all-sample number, −0.1065 against −0.0556. The recession
lives inside the blocks.

The wall is a **running bond**: band k+1's vertical mortar sits at the middle of
band k's blocks. With no column line inside a block, the only nodes in a band-k
block interior come from the union **row** at a band change, where band k+1's
mortar columns are injected — and the row below has nothing under them. The
Delaunay then covers the block with triangles whose three corners are all
mortar-deep. Measured at cam A: a triangle at (153.25, 127.5) (150.75, 127.5)
(126.75, 189.75) texels, corner heights h = 0.048 / 0.027 / 0.233, spanning a
57.5-texel block whose field reads h = 0.59. Cross-tabulated, the
plateau-by-field ∩ plateau-by-grid cell holds 149 571 samples at e−r p50
**−0.0799 u**.

**This also corrects §2c.** Its restricted-quadtree rule — "adjacent cells
differ by ≤ 1 level" — is **not satisfied** by the P2 lattice: a 2.5-texel mortar
cell abuts a 57.5-texel block cell, 4.5 levels apart. P2's
`level_jump_violations = 0` measured the row **split count**, not the cell sizes,
so the invariant was reported green on the wrong quantity.

## The ladder, and what each rung costs

All rungs are the identical height rule; only the lattice varies. cam A.

| arm | stone faces | plateau e−r p50 all / core | planes over ±0.005 (core) | min-ang p10 | < 1° | dz vs bare floor | tear px |
|---|---|---|---|---|---|---|---|
| **`--v4_cpb=1` (shipped)** | **60 606** | −0.0556 / −0.1065 | 10 of 33 | 2.41 | 8 | **2.91–4.84×** FAIL | **14** PASS |
| `--v4_cpb=2` | 110 249 | −0.0205 / −0.0234 | — | — | — | — | — |
| `--v4_cpb=4` | 227 348 | −0.0015 / +0.0001 | **0 of 50** | 15.04 | 0 | — | — |
| `--v4_band_union` | 124 716 | −0.0084 / −0.0075 | 10 of 36 | 2.65 | 3 | **0.74–0.78×** PASS | **18** FAIL |
| `--v4_band_union --v4_cpb=1.5` | 183 645 | worst plane −0.0189 / core worst −0.0032 | **0 of 46** | 4.40 | 0 | — | — |
| `--v4_band_union --v4_cpb=2` | 200 379 | −0.0030 / −0.0016 | **0 of 48** | 4.79 | 0 | — | — |

`--v4_band_union` gives every row the column lines of **every** block-row band,
which puts a line through every block interior; at that line the point is outside
its *own* band's runs, so it is a plateau node reading the max-pyramid — the node
the interior was missing.

**It shipped ON for one commit (b518d7a2) and was reverted by the tear battery
(f53dc4d6).** It takes the 54-pose battery from 14 to 18 hole px against a 15 px
cap, and the 5 new pixels are not the silhouette recession a displaced arm is
allowed to move: every one is an **interior gap** — all 8 neighbours rasterised
in both arms, all 8 stone in the control — and they are **collinear**, the
signature of a crack along one line (G2 (1457,49) (1378,70) (1299,91), spacing
(−79,+21); P5813 (1280,488) (1293,668) on `floor::mirUV`). Decomposed:

| pose | P2 lattice, amp 0 | band-union, amp 0 | band-union, amp 0.3 | no union, amp 0.3 |
|---|---|---|---|---|
| G2 | 0 | 0 | **3** | 0 |
| P5813 | 2 | 2 | **3** | 1 |
| P5854 | 0 | 0 | 1 | 1 |

so the G2/P5813 pinholes are the band-union **×** displacement interaction —
neither the lattice change alone nor the displacement alone makes one — and only
P5854's single pixel is displacement's own. The mechanism is consistent with the
union putting two column lines from *different* bands within a texel of each
other on one row, where the relief class flips between them and the surface takes
a 0.16 u step across ~1 texel; `kLineMergeTex` is 1.0 texel and does not see the
class. That is a defect in the change, not in the phase, and it is not local
enough to patch at the P3 gate.

## Two instrument notes

1. **`tools/nspace_relief.py` cannot run on this branch.** It imports
   `tools/refdiff_detect.py` and reads `*_refplane.txt` dumps from
   `--refplane_dump`; all three live only on `rev-dispfix` (d3f7a1ef). P3
   measures the same quantity as a **bake-time** census (`[V4-RELIEF]`): 9
   equal-area barycentric samples per emitted triangle, the surface's own height
   along the plane normal (the linear interpolation of its three vertices'
   displacements, exact because the whole face rides one normal) against the
   field's `d(u,v)` at the same UV, in nspace_relief's own classes (groove
   r < −0.03, bevel −0.03..0, plateau r ≥ 0) so the numbers are comparable to the
   −0.036 u it found. Stronger where it matters — no camera, no grazing 1/cos
   stretch, every plane, the whole surface rather than the visible pixels — and
   weaker in one way: its `r` comes from the bake's own sampler. The independent
   check on that is the dz row, which uses `DeferredDisplaceRef.cpp`'s separate
   implementation, and the two agree on the direction and the size.
2. **The block-interior mask needs no camera and no second reference arm.** A
   reference pixel is block interior when its shading normal is within 2° of its
   own face's plane normal, and the plane normal is recovered from the reference
   alone as the per-`faceId` **median** normal (the flat parts are the majority of
   every face's area). It calibrates itself: the bare wall reads **0.0142–0.0257 u**
   p50 through that mask across the corpus four, which is §0's independently
   stated reference noise floor of 0.016–0.035 u per pose.

Also on file, because it looks alarming in the battery output and is not a
defect: 31 of the 54 tear poses report `new_coverage=42049`. Those poses carry
`cam -`, which the battery replaces with the fixed `0,0,0,0,0,1` camera, so all
31 render **one** frame. At that framing the viewer looks *along* the floor
plane: the flat control draws the floor edge-on at zero height and the displaced
arm gives it relief, so its silhouette becomes a 39-px band at the horizon — all
42 049 px are `floor::mirUV` in y ∈ [540, 578], 40 037 of them with all 8
neighbours rasterised in the arm, and holes there are 0.

## What P4 inherits — and why it should not start yet

* The height rule of §2d, complete and cheap (6 ms), with its own census and its
  own gate reader, and a direction field that is the chart plane normal and
  nothing else (max deviation 1e-06°).
* Every ring still pinned at 0 and every P2 invariant intact, so §2e's
  offset-plane solve has exactly the mesh it was designed against.
* **An open ruling, `greets.displace.v4.ruling.p3_density`** (`waiting_on:
  gil-ad`): P3 cannot meet its own gate without a denser lattice, and each
  candidate breaches a different invariant — (a) ship the recessed default, (b)
  `--v4_band_union` and accept 5 interior pinholes plus 1.4× the faces, (c)
  `--v4_band_union --v4_cpb=1.5`, the cheapest arm whose every plane passes, at
  2.0× the old bake's faces, (d) `--v4_cpb=4` at 2.5×, or (e) find the
  band-union pinhole's local fix first.
* **P4 should not start on the shipped arm.** The corner rule reconciles the
  relief at a junction; on this arm the plateau it would reconcile is 0.1 u out
  of place, so a junction measurement taken now would be measuring the block
  interior's error, not the corner's.
