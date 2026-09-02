<!-- Provenance: written 2026-09-02 by the from-scratch attack agent (ledger writer subagent:attack) at Gil-Ad's request ("suggest how we should attack this issue from scratch ... what approach in general ... how to actually get a test that actually tests for the correct output ... how to actually get the correct output"). Copied verbatim from the session scratchpad by the coordinator; the coordinator corroborated section 0's mechanism against its own earlier census (ledger 41a4 supersede of a1588f2a3bda) and did NOT re-run the agent's pixel decode. Ledger: 64e87ccec15d 087a63970e4c 8fd6c6d41202 8e5c22727eec. The scratch paths it cites are session-local; the renders it rests on are reproducible from the commands it prints. -->
# Attacking the greets stone-displacement junction defect

Written 2026-09-02 by a subagent brought in cold. Everything below marked
**MEASURED** I ran or read today; everything marked **HYPOTHESIS** I did not
prove. Code claims carry `file:line`. Measurements carry the scratch path.

Scratch root: `/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/attack/`

---

## 0. The one thing that changed my mind, up front

**The black wedge at H6194 is not at a high-angle junction. It is at a
near-coplanar seam, between two faces of the same wall.**

At H6194 I dumped the depth plane, the per-pixel face-ownership plane and the
`[FACEID]` resolution table together (a combination nobody in the campaign
appears to have run):

```
cd /Users/gil-ad/work/revival-fog/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 FDS_XPAR_TRACE=1 \
  FDS_GREETS_CAM="22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849" \
  ../build/DEMO/DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
  --greets-displace --face_id_dump --force_xres=1920 --force_yres=1080 \
  --snapshot=greets@t=6194 --out=<dir>
```

Analysis scripts: `attack/probe.py`, `probe3.py`, `probe4.py`, `probe5.py`.
Reconstruction is validated: every lip pixel reconstructs onto the authored wall
plane x = 17.898 to within 0.04 u, using
`x_view=(px−CntrEX)·z/FOVX`, `y_view=(CntrEY−py)·z/FOVY`, `z=(0xFF80−zEnc)/g_zscale`
(`FDS/RENDER/DeferredSSAO.cpp:29`, `:697-698`; `FDS/RENDER/RENDER.CPP:1668`),
`FOVX=1728.0 FOVY=1296.0 CntrE=(959.5,539.5) g_zscale=395.636353`,
right = normalize(cross(worldUp, fwd)), up = cross(fwd, right).

**MEASURED, at H6194, shipping arm:**

| fact | value |
|---|---|
| unrasterised (z16==0) pixels | 5273, in 137 components |
| largest component | 4585 px, bbox x[1008..1155] y[201..924] |
| hole-boundary pixels with ≥2 distinct neighbour faces | 509 |
| …whose *most different* neighbour pair is < 5° apart | 23.6 % |
| …< 30° apart | **69.2 %** |
| …≥ 60° apart | 21.2 % |
| boundary pixels touching only `rooms::mirUV` faces | 71.7 % |
| boundary pixels touching both `rooms` and `rooms::mirUV` | 5.3 % |

The four faces that bound the big wedge are all `rooms::mirUV` with normals
within 6.3° of each other. The splayed door reveal (material `rooms`, u ≈ +0.5)
touches only ~88 of the 509 boundary pixels, at the very bottom.

So: the defect **sits on** the doorway jamb line, which is why it reads as "where
the high angle walls meet", but the seam that is actually torn is the *coplanar*
one — the far wall meeting the spandrel above the door head.

### The exact mechanism at H6194 (MEASURED, not inferred)

From `[FACEID]` (post-bake, world):

```
c57/fi259  (far wall)  A=(17.8670,6.2020,-63.9140)  B=(17.8400,4.8100,-62.9880)  C=(17.8980,6.2020,-62.9520)
c57/fi78   (spandrel)  A=(17.8980,6.2040,-62.9520)  B=(17.8980,4.9370,-62.9520)  C=(17.8670,6.2040,-62.5180)
```

From the vertex-provenance census over the jamb line
(`FDS_STONE_CENSUS_BOX="17.5,18.3,-63.2,-62.7,4.5,6.4"` +
`--greets_displace_junction_census`, `attack/h6194_boxhi/log.txt`), on the line
x=17.898, z=−62.952:

* y = 6.310, 6.257, 6.204 and 6.310, 6.256, 6.202 — `pin 1 free 0 hCnt 0` (two index families, both pinned)
* y = 4.937 — `pin 1 free 0 hCnt 0 orig 1` (an authored vertex, pinned)
* y = 4.810, 4.746, 4.683, 4.641, 4.640, 4.599, 4.598, 4.557, 4.556, 4.515, 4.513 — `free 1`, mitred
* **nothing at all between y = 4.937 and y = 6.202.**

`[STONE-MITRE] 'rooms' line 8/9` span `s[0.20,4.81]` — the mitre correctly stops
below the door head. It is not over-reaching.

So the far-wall sheet's boundary over that stretch is **one straight chord**
from the pinned corner (17.8980, 6.2020, −62.9520) to the mitred sample
(17.8400, 4.8100, −62.9880) — the latter moved 0.0683 u along (−0.849, 0, −0.527),
which is exactly −1 × the doorway mitre bisector. The spandrel's boundary over
the same stretch is the straight authored segment (17.8980, 6.2040, −62.9520) →
(17.8980, 4.9370, −62.9520).

Two curves, one seam. Evaluating the far-wall chord at y = 4.937 gives
(17.84529, 4.937, −62.98472); the spandrel's endpoint there is
(17.8980, 4.937, −62.9520). **Gap = 0.06204 u**, tapering to 0.0020 u at the top
(|fi259.C − fi78.A| = 0.0020). A hinge crack.

Independently measured from the depth reconstruction, lip to lip across the
4585-px wedge, 724 rows: **min 0.0056, p50 0.0306, p90 0.0523, max 0.1216 u**,
against a height-field amplitude range of ±0.164 u. That is a crack up to 74 % of
the full relief depth.

### Why the existing tear metric said "watertight"

rev-dispfix concluded the mesh was watertight at 99 % of hole pixels because
tri–tri distance read 0. **MEASURED:** for all five dominant hole-bounding face
pairs at H6194 the two triangles share (or nearly share) the hinge vertex, so the
*minimum* pairwise distance is ≤ 0.002 u — while the seam yawns to 0.062 u one
vertex away. **Minimum triangle-pair distance is structurally blind to a hinge
crack.** Any conclusion resting on it needs re-deriving.

### The mitre's share

**MEASURED** (`attack/h6194_nomitre/`, one flag apart):

| arm | z16==0 px | largest component |
|---|---|---|
| shipping | 5273 | 4585 px, 148 px wide |
| `--no-greets_displace_mitre` | 1444 | 373 px, **4 px wide**, same line, same y range |

The mitre does not create the crack; it *widens* it ~10×, because it moves the
last vertex of the far-wall ladder while the neighbour has no vertex to move.

---

## 1. Diagnosis: why this approach keeps failing (one paragraph)

**Continuity across a seam is a property of an EDGE; the bake only ever decides
things per VERTEX.** Two sheets stay joined only if (a) their two boundary
polylines are the *same point set* and (b) each shared point receives the *same*
displacement. `DisplaceStoneSubdiv` guarantees neither. Each sheet is
tessellated from its own UV grid, so a vertex can exist on one side and not the
other (the H6194 T-junction at y = 4.937; `[STONE-JUNC]` also reports 8
"SPLIT-VERTEX seam edges" and 154 "genuinely OPEN borders" on `rooms`), and each
vertex is then classified independently by a chain of ~8 predicates — pinned /
freed (`greets_displace_border_pin`, `_free_edge`), border-mean level
(`_border_mean` × `_border_mean_scale=0.40`), plane-normal ride vs smoothed ride
with three exception classes (`[STONE-PLANE]`: 833 corner verts, 600 position
twins, 10864 foreign-family contacts), groove-line snap (`[STONE-LINE]`: 31457 of
40034 snapped, 827 fallback), mitre override + a 0.06-u corner-band blend
(`DEMO/MeshOps.cpp:5955-5998`), then fold relaxation (`[STONE-FOLD]`: **1783
inverted faces relaxed** on `rooms` alone). The correctness condition is that all
eight predicates agree on both sides of every seam — a combinatorial condition
that no single flag can enforce and that every added flag makes harder. That is
why 46 commits of notch tapers took H6194 from 5072 to 478 holes without reaching
0, why the shipping arm still leaks 5273 px, and why "fix the corner rule"
(mitre, bisector, offset planes, mitre limit) has never converged: those levers
change the *value* of a displacement in a representation that keeps two curves
where the model has one.

---

## 2. Definition of correct output at a junction

State it so it can be computed with no reference to the bake.

**Inputs.** A set of planar authored polygons `F_i` with outward normal `n_i`,
affine UV map `φ_i`, a height field `h(u,v) ∈ [0,1]`, amplitude `a`, and a
reference level `m` (the mip mean).

**The displaced surface of one face.** `D_i = { q + n_i · a·(h(φ_i(q)) − m) : q ∈ plane(F_i) }`
— a height graph over the face's plane. `d_i(q) := a·(h(φ_i(q)) − m)` is the
offset; the *offset plane at height d* is `{x : n_i·x = n_i·q₀ + d}`.

**The solid.** The union over `i` of the slabs behind `D_i`. The correct visible
surface is the boundary of that union.

Now the three junction classes, defined by the **material dihedral** across the
authored edge `E = F_1 ∩ F_2`:

1. **Convex (exterior corner, dihedral < 180°).** Both graphs extend past `E`;
   the correct surface is their **intersection curve** — the point on the edge
   normal-plane that lies on *both* offset planes. Closed form
   `u = d·(n₁+n₂)/(1+n₁·n₂)` for equal offsets, the 3×3 system (rows `n₁,n₂,e`)
   for unequal ones, with a mitre limit `1/cos(θ/2)` where the corner degenerates
   (ledger `f713599ea11d`). Both faces are **trimmed** at that curve. The curve is
   *not* on `E` and is *not* on the bisector unless `d₁ = d₂`.
2. **Concave (reflex).** Same equation, opposite branch: the slabs overlap and the
   boundary of the union is where the nearer graph ends. The overlapping material
   behind is interior and must be discarded, not rendered.
3. **Coplanar seam (dihedral ≈ 0).** `n₁ = n₂`, so the solve degenerates to the
   identity: **the correct surface is simply continuous**, one graph, one height,
   no corner, no ride, no bisector, no level. Two coplanar faces of the same wall
   must carry *literally the same* displaced boundary curve.

**The splayed reveal (H6194).** The jamb line is *not one class*. Below the door
head (y ∈ [0, 4.937]) it is case 1 at 116.4°. Above it (y ∈ [4.937, 6.204]) it is
case 3, far wall against spandrel. **The correct output therefore requires that
the class switch happens at a point that exists in BOTH meshes** — i.e. the
authored vertex at y = 4.937 must be a vertex of every sheet that touches the
line. Today it is a vertex of the spandrel only. That is the whole bug, stated in
the language of the definition.

**Corollary that the campaign has never used:** the definition says nothing about
"free edges", "border means", "plateau shoulders" or "ride directions". Those are
implementation vocabulary with no counterpart in the model. Anything the code
does that cannot be phrased in the four sentences above is unjustified by
construction.

---

## 3. The test

Three layers. **Build them in this order** — the first needs no oracle at all and
would have caught H6194 in milliseconds.

### T1 (PRIMARY) — Seam-consistency audit on the mesh. No oracle, no rendering, bar exactly 0.

Dump the baked stone mesh once after `DisplaceStoneSubdiv`, then check a
predicate that shares no code with the displacement rule:

* build the half-edge structure of the emitted stone triangles;
* for each **authored** stone edge `E` (there are only 375 on `rooms`: 213 welded
  + 8 split + 154 open, per `[STONE-JUNC]`), collect the boundary polyline each
  incident sheet contributes;
* report, per authored edge: the two sides' vertex counts, the **symmetric
  Hausdorff distance** between the two polylines, and the count of vertices
  present on one side and absent on the other (T-junctions);
* report the mesh-wide use-count histogram: every interior edge must be used
  exactly twice.

**Metric:** max seam Hausdorff distance, in world units. **Pass: 0.000 exactly**
(bit-equal shared vertices), or ≤ 1e-6 u if the two sides index distinct-but-equal
positions for UV reasons (the `mirUV` clone split is a legitimate reason to
duplicate an index — it is never a reason to duplicate a *position*).
**T-junction count on stone: 0.**

*Why this and not a ray-cast oracle:* it is a predicate, not a comparison. There
is no tolerance to argue about, no second definition to get wrong, no camera, no
lighting, no percentile. It covers **every junction in the scene at once**, not a
pose list. It runs in milliseconds. And it is exactly the thing the model
guarantees and the bake does not.

**Wiring.** Needs one new dump: a `--greets_displace_mesh_dump=<path>` flag in
`FDS/Base/FeatureFlags.def` (default off, byte-null), written at the end of
`DisplaceStoneSubdiv`, carrying per emitted stone triangle the three world
positions, the three UVs, the material name, and per vertex its **authored parent
edge id / parent face id** (the bake already tracks `freeEdgeKA/KB`,
`recessOnly`, `mitreOf`, so the provenance exists in registers — it just is not
written out). Plus `tools/seam_audit.py`, ~200 lines.

**Effort estimate (HYPOTHESIS):** half a day for the dump + audit.

**A version you can run TODAY with no code change,** and which I used above: at a
pose, `--face_id_dump` + `FDS_SNAPSHOT_ZDUMP=1` gives you `A/B/C/UV/N` for every
face on screen plus the per-pixel owner, which is enough to run the audit on the
*visible* subset. Do this first as the acceptance test for the audit itself.

**Acceptance check for T1 itself:** it must reproduce, from the mesh alone,
the numbers I got from pixels — a seam on the jamb line with Hausdorff 0.062 u
and a T-vertex at y = 4.937. If it does not, the audit is wrong, not the bake.

### T2 — Coverage, control-referenced, screen space. Mostly already built.

`tools/v4_tear_cover.py` + `tools/v4_tear_battery.sh` already do exactly this
shape of test (control render vs arm render, z16==0 where the control had stone =
hole). Retarget it: control = `--no-greets_displace` (the bare wall), arm = the
shipping bake. **Metric:** count of pixels where the bare arm rasterises stone and
the displaced arm rasterises nothing, excluding pixels within 1 px of a depth
discontinuity in the bare arm (a receding silhouette may legitimately open a
pixel; a wall interior may not). **Pass: 0 per pose.**

**Pose set.** H6194 (the named pose) + one pose per mitre line — from the census
there are exactly 10: lines 0–3 (dir +z, sloped-vs-vertical joints, s ∈ [−24.4,
−18.9]), lines 4–7 (dir +y, s ∈ [3.21, 6.31], wall normals 63° apart), lines 8–9
(the two doorway reveals, s ∈ [0.20, 4.81]) — plus the 8 split-vertex 91.1° seams
and controls camA t=5965, corridor t=5534, curved t=2845, doorway t=5963. Add
`docs/tears_poses.txt` (54 poses) as the regression net.

**Effort (HYPOTHESIS):** half a day, mostly writing the pose list.

### T3 — Position oracle, offline Python, shares no code with the engine.

Only after T1 and T2 read 0. Export before the bake: the authored stone faces
(positions, UVs, material, plane), the height map bytes at the bake mip, `amp`,
`mip`, `mipMean`, and the camera. Then in Python ray-cast **the §2 definition**:
per pixel, intersect the ray with each face's slab, DDA-march the height field in
texel space, take the union, and apply the mitre trim **at convex edges only**
(the ledger already measured that the convex-only trim is the correct one:
`8ab9ddcb58e2`, miss px 4009 → 36 at cam A). Compare against the bake's z16.
**Metric:** |dz| p50/p90 over stone pixels and over a crease band, with the
**reference-coverage tail count reported beside it** — the ledger trap
`a149cc880bbe` shows the p90 sits inside that tail by construction at some poses.

**Do NOT use `--greets_displace_ref` as the oracle.** His own verdict
`a0e7dfd23ca4` is that it is not ground truth ("at our problematic wall
connections the height doesn't always agree from both sides"), the cam-A far wall
is broken in *every* reference membership arm (`6d633f210431`), and it costs
349–505 ms/frame (`75a1ca5fcd65`). It is a valuable *second implementation* —
agreement corroborates, disagreement localises — not an authority.

**Effort (HYPOTHESIS):** 1–2 days plus the export, and a real risk of
re-litigating the definition. That is why it is third.

### What already exists vs what must be added

| need | status |
|---|---|
| per-pixel depth (`z16`) | **exists**, `FDS_SNAPSHOT_ZDUMP=1`, `DEMO/Snapshot.cpp:825` |
| per-pixel face owner + face table (positions/UV/N) | **exists**, `--face_id_dump`, `DEMO/Snapshot.cpp:889` |
| per-pixel material | **exists**, `FDS_SNAPSHOT_GBUFDUMP=1`, `:866` |
| per-vertex provenance in a box | **exists**, `--greets_displace_junction_census` + `FDS_STONE_CENSUS_BOX` |
| whole-scene junction census | **exists**, `[STONE-JUNC]` |
| control-referenced hole detector | **exists**, `tools/v4_tear_cover.py` |
| **whole baked mesh + authored parentage dump** | **MISSING — this is the gap** |
| **authored pre-bake faces + height map export** | **MISSING** (needed only for T3) |

---

## 4. Approaches to get the correct output

### A. Shared seam ladder — make the two curves one curve (RECOMMENDED)

Before displacement, build for every authored stone edge **one** canonical sample
list: both endpoints, every authored vertex either side contributes (this alone
fixes H6194's y = 4.937 T-junction), every mortar-groove crossing of *both* sides'
UV maps, then equal-arc fill. Create each sample **once**; have both sheets index
it. Displace each sample once, by §2: the plain field for a coplanar seam, the
offset-plane solve at the dominant owner's height for a crease.

* *Correct by construction* — one vertex cannot disagree with itself. T1 = 0.
* *Cost*: bake only. 375 authored edges on `rooms`; against a 476.5 ms old bake
  (ledger) this is noise. Faces: a few hundred added (v4's P4 measured +0.93 %
  for the same operation). **Runtime: 0 ms.** No filler change, no per-pixel cost.
* *Risk*: the old bake's border machinery is per-vertex *and per-side*. Making
  the sample shared forces the seven classifiers to collapse into one decision
  per seam — that is the actual work, and it will move pixels on the wall, so it
  needs his eye, not a byte gate. Second risk: `[STONE-PLANE]` reports 600
  "POSITION TWINS" and the `mirUV` clone split duplicates indices deliberately;
  "one sample" must mean *one position computed once, stamped into every clone*,
  not one index.
* *Proof*: T1 max Hausdorff 0.000 and 0 T-junctions; T2 0 holes at H6194 and the
  10 mitre lines; `--greets_displace_amp=0` byte-identical to today's amp=0 (the
  retessellation must be relief-neutral); `tools/render_gate.sh` 4/4 flag-off.
* This is exactly what v4's P4 rings do (`b2dedcb218bd`) — applied to the **old
  bake's mesh**, which he has ruled is very good everywhere else
  (`d98c40a5f377`), instead of to a new lattice he has rejected.

### B. True mitre — re-trim both sheets to the intersection curve of the two displaced surfaces

* *Correct by definition* for creases (§2 case 1).
* *Cost*: a 3-D boolean over ~90 625 displaced stone faces at bake time.
* *Risk*: float robustness on near-tangent surfaces; and — decisive — **it does
  nothing for the 69.2 % of measured hole boundary that is near-coplanar.** A
  mitre is undefined when `n₁ = n₂`. **Refuted by the measurement in §0.**

### C. Per-pixel relief at junctions, or globally (`--pom_shell_weld`, POM)

* *Correct output*: only if the shell that carries the march is itself watertight,
  so it does not remove the problem, it relocates it.
* *Cost*: this is a software rasterizer. The in-tree per-pixel march of the same
  definition runs 349–505 ms/frame at best (`75a1ca5fcd65`) — two orders outside
  "single-digit ms". A junction-only march still needs a per-pixel branch in the
  filler hot loop.
* *Risk*: `pom_shell_v1` is already in the corpus as his "broken" (`9e4ee1e7d0d9`).
* *Verdict*: not a delivery path on this engine. Keep the ray-march as an
  offline instrument only.

### D. Pin the arris straight at every junction and ship

* *Correct output*: a *defined* surface (the graph clamped to 0 in a band around
  every authored edge), watertight by construction.
* *Cost*: zero. It is `--no-greets_displace_free_edge`, which I confirm reads
  **0 unrasterised pixels** at H6194.
* *Risk*: the dead-straight silhouette that started this campaign. **But** — the
  original complaint was about a *long* straight arris from pinning whole
  borders; a *one-cell* pin band with relief resuming immediately is a different
  look and has never been shown to him. And the ledger's own yardstick says the
  bake is *further* from the reference definition than the flat wall at 7 of 8
  poses (`f47faa153876`), so the border relief is not obviously buying fidelity.
* *Proof*: T1 = 0 and T2 = 0 by construction; the look is his call on a deck.

### Recommendation: **A**, with **D** built the same day as the bisect control.

The measured defect is a *representation* defect, not a *rule* defect. Fixing the
representation makes the whole bug class unrepresentable and is testable by a
predicate with a bar of exactly 0 and no oracle. D exists so that if A's look
regresses, there is a shipping fallback that is provably watertight, and so that
every A/B has a zero-hole endpoint to bisect against.

### First three steps

**Step 1 — build T1 and run it on the shipping bake (half a day).**
Add `--greets_displace_mesh_dump` to `FDS/Base/FeatureFlags.def` (never raw
getenv), write the mesh + authored parentage at the end of `DisplaceStoneSubdiv`,
write `tools/seam_audit.py`.
*Check:* the audit, from the mesh alone, reports the jamb seam at x=17.898,
z=−62.952 with symmetric Hausdorff ≈ 0.062 u and a T-vertex at y=4.937, and a
scene-wide max/histogram. If it does not reproduce my pixel-derived number, stop
and fix the audit.

**Step 2 — make the seam ladder shared (a day).**
Canonical per-authored-edge sample list, created once, indexed by both sheets,
with a sample at every authored vertex of either side and at every mortar-groove
crossing of both UV maps.
*Check:* T1 max Hausdorff 0.000, T-junctions 0; T2 holes 0 at H6194 and the 10
mitre lines; `--greets_displace_amp=0` byte-identical to today's amp=0;
`tools/render_gate.sh` 4/4 with `--greets_displace` off.

**Step 3 — one height rule per seam class, and delete the per-side classifiers on
the seam (half a day).**
Coplanar → the plain field, no special case. Crease → offset-plane solve at the
dominant owner's height (`1c196c8c5cea`, `f713599ea11d`).
*Check:* T2 = 0 over the full pose list; `[STONE-FOLD]` inverted-face count on
`rooms` falls from 1783 toward 0 (a bake that needs 1783 self-inversions relaxed
is not producing a surface); then a wipe/A-B deck at H6194 + 3 other junctions for
his eye.

---

## 5. Delete rather than tune

1. **`--greets_displace_mitre`** and its satellites `--greets_displace_block_level`,
   `--greets_displace_geom_bisector`, `--greets_displace_band_ladder`,
   `--greets_displace_border_v2`. MEASURED: the mitre is responsible for 3829 of
   the 5273 unrasterised px at H6194 (5273 → 1444, one flag apart), and the class
   it targets carries only ~21 % of the hole boundary. Its grouping key is an
   **infinite line** — quantised direction + foot point at 1/8 u,
   `DEMO/MeshOps.cpp:5403-5412` — with no notion of the *segment* over which the
   two walls actually abut, and its whole job (make both borders re-samplings of
   one curve) is what step 2 does properly and cheaply. Superseded, not tuned.
2. **`--greets_displace_fold_relax`.** `[STONE-FOLD]` reports **1783 inverted faces
   relaxed** on `rooms` (8 halving passes) and 57 on `floor`. This is a
   symptom-suppressor that also perturbs the surface (189 hole px move with it:
   5273 → 5084 flag-off). Keep it only until step 2 lands; then it must read 0
   and go. If it does not read 0, step 2 is not done.
3. **`--greets_displace_border_mean` + `--greets_displace_border_mean_scale=0.40`.**
   The flag text itself calls 0.40 "A MEASURED CALIBRATION CONSTANT". A magic
   fraction of a mean level has no counterpart in §2; it exists to make the two
   sides of a seam look similar without making them equal. Delete with step 3.
4. **`--greets_displace_seam_weld`.** MEASURED byte-identical at H6194 (5273 →
   5273 with `--no-greets_displace_seam_weld`). Either it earns its keep at
   another pose — show which — or it goes.
5. **The `--displace_viz` overlay's back-face cull.** `FDS/RENDER/WorldAabb.cpp:566-569`
   culls on `F.N`, the stored authored normal, which follows the FLD's
   inconsistent winding — so the x = 17.898 wall that carries this entire defect
   draws **no wireframe at all** while being displaced. An instrument that
   silently omits its subject is worse than none. Cull on the rendered winding or
   delete the overlay.
6. **The min-triangle-pair-distance tear metric** (rev-dispfix). MEASURED: at
   H6194 every dominant hole-bounding pair shares a hinge vertex, so the metric
   reads ≤ 0.002 u on a 0.062 u crack. It reported "watertight at 99 % of hole
   pixels" on a hole you can see from across the room. Replace with T1's seam
   Hausdorff, and re-derive anything that rested on it.

---

## 6. What I did not resolve

* **HYPOTHESIS, untested:** that the far-wall sheet carries no boundary sample
  between y = 4.937 and y = 6.202 *because* the subdivision follows the height
  map's UV grid per sheet and that stretch falls inside one block course. I
  measured the absence, not the cause.
* **HYPOTHESIS:** that the same T-junction pattern explains the other 9 mitre
  lines and the 8 split-vertex seams. T1 answers this in one run — that is
  precisely why it is step 1.
* I did not measure bake time, face count, or any runtime cost of the proposed
  change. The "0 ms runtime" claim in §4A is structural (no filler change), not
  measured.
* I did not look at whether `mirUV` clone splitting interacts with the shared
  sample. Flagged as the second risk in §4A.
