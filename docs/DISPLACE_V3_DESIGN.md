# DISPLACE v3 — clean-room design for junction-correct stone displacement

> Status: DESIGN ONLY (2026-08-25). No implementation exists. Implementation is
> gated on Gil-Ad's verdict on the compare-notes sketch
> (`docs/img/bulge3/corner_intent_sketch.svg`) and on the open questions in §8.
>
> Clean-room rule honored: this design was produced WITHOUT reading the
> internals of `DisplaceStoneSubdiv` or the classifier/border/mitre machinery
> in `DEMO/MeshOps.cpp`. Inputs were: the call-site interface
> (`DEMO/GREETS.CPP:2019`), the public flag documentation in
> `FDS/Base/FeatureFlags.def`, the base structures (`TriMesh`/`Face`/`Vertex`),
> the height-map source facts (`Runtime/TEXTURES/greets_wall_h.png`, 4×4 block
> grid, red channel via `MakeHeight8`), and a purpose-built scene census
> (`--greets_junction_census`, output in `docs/jcensus_greets.txt`).

## 1. Problem statement

Gil-Ad's datum (2026-08-25): the bulge is a **wall-junction defect**. Wall
connections below roughly ~240° (his estimate, uncertain) render okay; above
it — the connection at his t=5965 pier pose — "the connection between the
walls gets fucked up - it uses the wrong calculation, probably due to the
angle." Faces adjacent to such junctions lean/bow (the pillow/bulge read);
secondary defect: gaps/slits near the same walls, and the long-standing
wall-base slits.

**Angle convention (pinned):** the junction angle θ is measured on the
**visible (back-cull) side** — swept through the region where a viewer can
see both faces. θ = 180 is a flat continuation, θ < 180 an interior corner
(inside of a room, wall-on-floor), θ > 180 an exterior/reflex corner (the
pier arris). §7 records a measured trap: the engine's `A,B,C` winding cross
product is the **anti-visible** normal (the floor's winding normal is
(0,−1,0) while its visible side faces up) — any angle math built on the raw
winding normal computes 360−θ, which is precisely a "wrong branch above a
threshold" generator.

**Interface-level corroboration** (documented behavior, not code reading):
`FeatureFlags.def` documents the current bake as displacing "each interior
vertex along its **smooth vertex normal**", with patch-border verts pinned.
On a piecewise-planar shell, smoothed vertex normals at/near a crease are
mitre directions applied to NON-mitre verts: every vert that blends two
panels' normals drags its face off its wall plane, and the error grows with
the junction's deviation angle — an angle-dependent lean, matching the
datum. (Hypothesis from documentation; the design removes the mechanism
regardless of its exact current form.)

**Defect classes this design must kill:**
- **J-LEAN**: faces near reflex junctions lean off their wall plane (the bulge).
- **J-SLIT**: junction gaps — two sides displaced by independent bakes fail to
  meet (wall↔wall and wall↔floor; the base-seam gash family).
- **J-BASE**: the wall/floor junction treated as a special case instead of the
  same junction mechanism.

## 2. Geometric model — the offset-shell bake

The scene's stone geometry is a **piecewise-planar shell**: maximal planar
panels joined at straight junction lines (measured: 52 panels, 117 junction
edges, zero >2-plane edges, all in one mesh — §7). Displacement of such a
shell is polygon/polyhedron **offsetting**, and offsetting has a century of
correct machinery (mitred offsets, straight skeletons, miter limits). v3 is
that machinery applied per height sample:

1. **Panels.** Cluster target-material faces into maximal coplanar panels
   (normal dot > 0.9995, |d| within 0.05 — the census clustering, which
   produced exactly the expected 52 planes). Each panel carries its
   **visible-side unit normal** `n` (winding cross NEGATED — see the trap),
   its UV frame, and its border loops.

2. **Junction graph.** Every mesh edge shared by faces of two panels is a
   junction edge; colinear runs merge into junction LINES. Each junction
   stores its two panels (A,B), the edge direction, and its class:

   > **INVARIANT (named): the convex/reflex branch is decided by the sign of
   > `uB·nA`** (uB = B's in-plane direction perpendicular to the edge,
   > pointing into B's face; nA = A's visible normal): θ = atan2(uB·nA,
   > uB·uA) mapped to [0,360). `dot(nA,nB)` alone CANNOT distinguish θ from
   > 360−θ and must never classify a junction. This is the census formula;
   > it is validated against the scene (interior room corners read 90,
   > wall-on-floor reads 90, pier arrises read 270 — §7).

3. **Panel interiors.** Every interior vertex displaces along its **panel
   plane normal**, never a smoothed vertex normal:
   `P' = P + a·(h(u,v) − h0) · n_panel`.
   A displaced interior face is a parallel offset of its wall by
   construction — **J-LEAN cannot exist on panel interiors at any
   tessellation density**. (This is why density levers never touched the
   bulge: density changes the sampling of a field applied along the wrong
   directions.)

4. **Junction rings — watertight by ownership, not by coincidence.** Each
   junction line OWNS one shared ring of verts; both panels index into that
   ring. There are no "two borders that must happen to coincide" — J-SLIT is
   removed by construction, the same way the cross-bake guard chased it
   after the fact. Ring vert offset:
   - direction `d = (nA + nB) / |nA + nB|` — the angle bisector. This form
     is **branchless-correct for both interior and reflex** junctions (the
     normal sum always points into the visible side); the classification
     invariant is still computed and asserted, because downstream rules
     (bevel orientation, grout wrap) need it.
   - magnitude `s = 1 / cos(δ/2)` where `δ = angle(nA, nB)` (the deviation),
     so BOTH faces remain exact parallel offsets and meet on a straight
     arris (the mitre join). For the scene's orthogonal corners δ=90,
     s=√2 ≈ 1.414 — tame.
   - **miter limit with bevel fallback**: if `s > kMiterLimit`
     (flag, default 2.0 — the stroke-rendering rule, s = 1/sin(θ_seg/2) in
     SVG's formulation), do not mitre: split the ring vert into one vert per
     panel, each offset along its own panel normal, and stitch a **bevel
     strip** between the two. Nothing in the current scene needs it
     (δ ≤ 90 everywhere measured), but razor junctions must degrade to a
     chamfer, not to an exploding spike. The bevel strip doubles as the
     chamfered-arris LOOK option (§8 Q4).

5. **One height value per ring vert.** Both panels sample the SAME scalar at
   a shared ring vert (the crack-free rule from the tessellation
   literature): the junction line is parameterized by arclength `t`; the
   bake evaluates `hA(t)` and `hB(t)` in each panel's UV frame, takes ONE
   agreed value `H(t)` (if the authored maps disagree across the joint,
   average once — the ring stores a single value, so the surfaces agree
   whatever the maps do), and uses it for the single ring vert. Displaced
   position: `P'(t) = P(t) + a·(H(t) − h0) · d · s`.

6. **Tessellation.** Per-panel regular grid in panel UV, cell borders
   snapped to the 4×4 block grid (cells-per-block a flag, as today).
   Junction ring density is owned by the junction — the max of its two
   panels' border densities; both panels conform their border rows to the
   ring. No hanging nodes (T-junctions) by construction.

7. **Normals.** Face normals re-derived from displaced geometry; the
   existing `MakeFacesIndependentByAngle(30°)` crease pass stays the vertex-
   normal policy (junction rings produce honest hard creases; block-top
   bevels below 30° stay smooth).

## 3. Sampling model

Height source is per material (`greets_wall_h.png` for 'rooms'), single
channel, finest mip unless flagged; each panel samples strictly in **its
own UV frame** — no world-space projection, no cross-panel filtering. At
junctions the shared parameterization of §2.5 is the ONLY place two frames
meet, and it meets in one scalar. Block-grid alignment of junction lines
(junctions falling on grout lines of the 4×4 grid) is expected but is
**verified at bake time** (M1 logs each junction's block-phase); misaligned
junctions don't break watertightness (the ring is still shared), they only
decide WHERE the grout lands relative to the arris — a look question (Q3).

## 4. The wall/floor junction is not special

The census: every rooms↔floor junction in the scene is an interior ~90°
line (33 edges, ~333 units of length, bins 80–120 — `docs/jcensus_greets.txt`).
Under v3 it is simply a junction ring between a wall panel and a floor
panel: one shared ring, mitre direction `(n_wall + n_floor)/|·|` (≈ 45°
into the room), one height value per ring vert. The historical base-seam
slits are the two-independent-bakes disease (`DisplaceStoneSubdiv` is
called once per material — `DEMO/GREETS.CPP:2019-2020` — so the shared line
is baked twice); v3's per-JUNCTION ownership makes the class unrepresentable.
Base look profile (bed joint groove vs pinned-flat base) is Q5.

## 5. Acceptance geometry

At Gil-Ad's poses — t=5965 cam A
`22.5084476,3.87992334,-61.8882256,-0.20672597…` (full values in
`docs/greets_review_poses.txt`, "the bulge" block) and cam B — plus the
15-pose battery:

- **Arris straightness (J-LEAN kill):** every displaced junction ring fits a
  straight line within ε = 1% of block pitch (measured on the baked verts,
  not pixels), and each displaced face's normal deviates < 0.5° from its
  panel's offset plane.
- **No slits (J-SLIT kill):** the face-plane/z-hole scan (`--face_id_dump`)
  at all battery poses shows zero new no-face pixels vs the undisplaced arm.
- **Junction-class coverage:** at least one named representative line per
  measured census class is inside some battery frame — interior 80–120
  (e.g. planes(2,4) vertical at (49.4, 0..18.5, −49.4)), obtuse 120–170,
  gentle reflex 200–230 (planes(7,10) at (12.7, 0..4.9, −54.9) — the line
  nearest his cams), sharp reflex 250–280 (the z=−37 colonnade arrises).
- **The look gate is his eye**, not the metrics; the metrics only decide
  whether a build is worth showing him.

## 6. Implementation plan (staged, each stage look-gated)

New flag `greets_displace_v3` (default OFF, byte-null when off), coexisting
with the old bake for A/B. Old flags untouched.

- **M1 — graph, no displacement.** Panel clustering + junction graph +
  block-phase log + a `--displace_viz`-style overlay coloring panels and
  junction classes. Gate: census counts reproduced; overlay renders sane at
  cams A/B.
- **M2 — interiors only.** Panel-normal displacement, junction rings PINNED
  at zero. Gate: zero lean anywhere (including the pier); slits allowed at
  rings (they're pinned); the bulge must already be dead on panel faces.
- **M3 — mitred rings.** Shared rings + mitre + miter-limit bevel. Gate:
  straight arrises at the 250–280 class and the 200–230 class; zero
  wall↔wall slits; his look pass on the pier.
- **M4 — floor rings.** Wall↔floor junction rings. Gate: zero base slits at
  the battery; base look per Q5.
- **M5 — his acceptance** at the full arm, then the default-flip decision.

Cost note: v3's face count is driven by the same cells-per-block dial as
today; the junction rings add O(junction length × ring density) verts only.
Perf is measured at M3/M4 with the standard three-arm protocol before any
default flip.

## 7. Scene junction census (MEASURED, `--greets_junction_census`)

Instrument: `DEMO/GREETS.CPP` (`GreetsJunctionCensus`, flag-gated, exits
before the first frame). Full output: `docs/jcensus_greets.txt`.

- 196 'rooms' + 30 'floor' faces → **52 planes, 117 junction edges, 0
  complex (>2-plane) edges, 0 cross-mesh edges** — the panel/junction model
  in §2 fits the data exactly; no T-junction or multi-mesh machinery needed.
- **Winding trap (measured):** the floor's `A,B,C` winding cross is
  (0,−1,0) — anti-visible. All visible-side math must negate the winding
  cross. The census does; the design mandates it.
- rooms↔rooms visible-angle histogram: interior 80–120° (29 edges, len 172);
  obtuse interior 120–170° (33, len 97); **gentle reflex 200–230° (5, len
  23); sharp reflex 250–280° (19, len 51)** — and **nothing between 230 and
  250**: Gil-Ad's "~240" threshold estimate sits exactly in the empty gap,
  so both "the 250+ class breaks" and "everything reflex breaks" are
  consistent with his words; the design handles all classes exactly, so the
  distinction only matters for acceptance emphasis.
- rooms↔floor: entirely interior 80–120° (33 edges, len 333). No reflex
  base lines exist.
- Named lines for acceptance: the junction lines nearest his t=5965 cameras
  are planes(7,8) at (9.9, 0..4.9, −49.4) ≈ 207° and planes(7,10) at
  (12.7, 0..4.9, −54.9) ≈ 212°; the sharpest arrises are the 270° colonnade
  at z=−37 (x ∈ {−9.9, −4.9, −2.5, 2.5, 4.9, 9.9}) and the room shells
  (e.g. (49.4, 0..18.5, −49.4)). → Q2 asks him to place his pose's broken
  junction on this list.

## 8. Open questions for Gil-Ad (block M3+; M1–M2 can start on a sketch OK)

- **Q1 — corner treatment:** sharp mitred arris with both faces parallel to
  their walls (sketch panel 1/4 left), or interlocking quoin blocks
  alternating per course?
- **Q2 — name the broken junction:** the line nearest your t=5965 cams
  measures 207–212° visible-side (bbox (12.7, 0..4.9, −54.9)); the scene's
  sharpest class is 270°. Is the junction you call broken this ~210° one,
  or a 270° arris also in frame? (Decides acceptance emphasis, not the
  algorithm.) If you measure the angle differently than §1's convention,
  one concrete number for one named line calibrates it.
- **Q3 — grout at the arris:** does a grout groove wrap around the corner
  (recessed vertical joint ON the arris), or does the corner block run
  continuous around the bend (as sketched)?
- **Q4 — chamfer option:** the miter-limit bevel strip can double as a
  small chamfered arris on EVERY reflex corner (dressed-stone look). Want
  it as a dial, or sharp arrises only?
- **Q5 — the base line:** wall meets floor with a recessed bed joint
  (groove running along the base), or flat/pinned as today?
- **Q6 — amp semantics:** keep `greets_displace_amp` world-unit scaling and
  the zero-mean (h−h0) convention as-is?

## 9. Survey — what each source contributes and what v3 takes

- **Lee, Moreton, Hoppe — “Displaced Subdivision Surfaces”, SIGGRAPH 2000**
  (https://hhoppe.com/proj/dss/). Scalar displacement over a SMOOTH domain
  along the domain's smoothed normal field — correct where the domain is
  smooth, and exactly the mechanism that tilts faces when the domain has
  hard creases. v3 takes the scalar-field-over-domain framing and the
  lesson in reverse: our domain is piecewise-planar, so the displacement
  frame must be the panel plane, not a smoothed normal.
- **Nießner, Loop — “Analytic Displacement Mapping using Hardware
  Tessellation”, ACM TOG 2013**
  (https://niessnerlab.org/papers/2013/3analytic/niessner2013analytic.pdf).
  Crack-free displacement by making shared edges evaluate to identical
  values on both sides (consistent seam treatment, mip-consistent
  sampling). v3 takes the principle and strengthens it structurally: shared
  junctions don't just EVALUATE consistently, they are one owned vertex
  ring, so agreement is by identity rather than by equal arithmetic.
- **Aichholzer, Aurenhammer et al. — straight skeletons; CGAL “2D Straight
  Skeleton and Polygon Offsetting”**
  (https://doc.cgal.org/latest/Straight_skeleton_2/index.html); **Palfrader,
  Held — “Computing Mitered Offset Curves Based on Straight Skeletons”**
  (https://www.tandfonline.com/doi/full/10.1080/16864360.2014.997637).
  Mitred offsets keep corners as corners, and the straight skeleton is the
  exact machinery for offset degeneracies (self-intersection of offsets at
  large depths). v3 takes the mitred-offset corner rule for junction rings;
  the full skeleton is out of scope because grout depths (≤ ~0.07 u) are
  far below the scene's feature radii — recorded as the known limit.
- **SVG/CSS `stroke-miterlimit`**
  (https://developer.mozilla.org/en-US/docs/Web/SVG/Attribute/stroke-miterlimit).
  The production-hardened miter rule: mitre length grows as 1/sin(θ/2);
  past a limit, fall back to bevel. v3 adopts the same limit-plus-bevel
  contract for junction ring verts (limit flag, default 2.0).
- **Thonat et al. — “Tessellation-Free Displacement Mapping for Ray
  Tracing”, ACM TOG 2021**
  (https://dl.acm.org/doi/10.1145/3478513.3480535). Modern context: correct
  displacement silhouettes without baked tessellation. Not applicable to a
  CPU rasterizer's baked mesh, but its seam handling reinforces the
  shared-evaluation requirement; noted for the record.
