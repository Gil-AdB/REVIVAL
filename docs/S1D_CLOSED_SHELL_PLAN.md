# S1d — Closed shell + cross-patch march continuation

> **CORRECTION 2026-08-06 — every "+54.5 ms" / "way too slow" statement about
> `--greets_displace` in this file is STALE by 7.4×.** Re-measured from scratch
> on fog-wt tip: the tessellated arm costs **+7.3 ms** over flat POM at t=5780
> (and is CHEAPER than the recess-shell arm at three of six review poses). The
> retirement number predated `9b6d70d --tile_bbox_cull` (which landed 1 h 40 m
> after the edge-carve commit and measures −12.5 ms of delta on its own),
> `a1f89d4 --xfrm_soa_inline` (−2.0 ms) and `799c808` (the faceless mesh was
> **84.3 % of that arm's shadow verts**), and the mirror clone was
> re-transforming the whole tessellated wall (−5.9 ms, now
> `--greets_displace_flat_mirror`). Full tables, look crops and gates in
> **`docs/ENVDYN_DISPLACEMENT_PLAN.md` §ADDENDUM 2026-08-06**. Leave the
> historical numbers below as written; do not reason from them.


Status: ACTIVE PLAN, written 2026-08-05. This is the design the campaign's
evidence converged on. Read `docs/S1_DISCREPANCY_INVENTORY.md` (evidence,
classes, converged reference, P0/P1/P2-A results) and
`docs/DISPLACEMENT_RESEARCH.md` §3.3 before touching code.

## Why this exists — the tension no knob can resolve

Two user-reported defects, measured, are the same defect:

| symptom | arm | measured |
|---|---|---|
| black gashes between wall panels, bar in the mirror | lid shell | 98,371 void px @t=5743 (tessellation: 3) |
| grazing smear, mortar dragged into ribbons | recess-only | 20,244 px @t=5958 — *the same pixels*, clamped instead of killed |

Proof they are one thing: at t=5958 recess-only with `--pom_recess_edge=0`
(clamp) has **0 void**; the identical arm with `=2` (discard) has **20,244**.
The march has no valid answer once a ray leaves its patch's UV domain; killing
gives holes, clamping gives smears. Neither is correct.

The cap is the same tension seen from the other side: `--pom_shell_cap` bounds
lateral travel. Cap 2 keeps rays inside their patch (few holes, grazing depth
wrong by up to 0.945 world). Cap 16 resolves grazing (holes double: 16,539 →
34,944 px in the coordinator's measurement, 68,122 → 97,329 in the P2-A
agent's — both directions agree, magnitudes disputed, see §10.1 of the
inventory). **You cannot have correct grazing and no seam artifacts with an
open patch.**

Third defect, user-reported, that no amount of march work can fix: recess-only
has no see-through in the stone valleys — nothing rises above the authored
plane, so a stone can never occlude the mortar beside it.

## The literature's answer (research §3.3)

Hirche '04 / shell maps '05 raster a **closed shell volume** — top *and* side
faces — and march inside it. A ray exits only where the surface genuinely ends,
i.e. at a true silhouette, and discarding there is correct: you are meant to see
what is behind. Our implementation rasters only the top lid of a box and
substitutes a UV-box test for the missing sides, so it also "exits" at INTERNAL
seams where the surface continues — and discards there.

`DISPLACEMENT_RESEARCH.md` §6 non-recommended the prism machinery because "our
flat quads reduce it to S1b". That is true for ONE quad and false for a wall
built of many small ones: greets' wall patches are 0.4–2.1 UV tiles wide while a
grazing ray travels ~0.24 UV, so rays cross patch boundaries constantly and must
be handed from one patch to the next.

## The design

Two independent pieces. Either helps alone; together they remove the class.

**A. Cross-patch march continuation (no geometry change).** When a ray leaves
patch A's domain without a hit, transform its current UV + remaining slab height
into patch B's chart and CONTINUE the march there. Requires per-patch-edge
neighbour links with a UV transform. Bounded hop count (start at 4) with a
defined terminal action. Works in BOTH lid and recess modes, and in both it
converts a hole/smear into correct relief.

**B. Side faces at true boundaries (geometry).** Where a patch edge has no
neighbour — the wall genuinely ends — extrude side quads so the shell is closed.
The silhouette becomes the shell's, protrusion is capped legally, and discard at
those faces is correct rather than a bug. This is what restores stones standing
proud with see-through valleys.

### Seam classification — the gating question

Not every patch boundary is the same:

- **Coplanar continuation** — the surface carries on in the neighbouring patch,
  same plane. A ray must continue; a hole here is always wrong. Expected to be
  the large majority (patches are split by mesh topology, not by geometry).
- **Angled junction** — a corner, another wall. The ray leaves this surface. Two
  sub-cases: it should enter the neighbouring wall's shell (continuation with a
  chart change), or it should exit to the scene (true silhouette).
- **True boundary** — the wall ends (doorway jamb, free edge). Exit is correct;
  this is where side faces belong.

**Stage 1 exists to measure this split.** If most seams are coplanar
continuation, (A) alone fixes most of the reported defects and (B) is a look
upgrade. If most are true boundaries, (B) is the priority. Do not assume.

> **ANSWERED 2026-08-05 by S1d-1 — and the expectation above is WRONG for
> greets. Read §S1d-1 at the end of this file before acting on anything in this
> section.** Measured over all 13 review poses: of the 800 513 pixels the march
> cannot answer, **0.004 % (31 px) sit at a coplanar seam**; 72.9 % sit at a
> CONVEX angled ridge, 15.1 % at a concave fold, 11.9 % at a true boundary.
> Coplanar continuation is already shipping as `--pom_shell_merge_uv`'s sibling
> boxes (worth 70 585 px, and the coplanar UV transform is the IDENTITY).
> **(B) dominates, not (A).**

## Stages, each independently gated

**S1d-1 — topology + classification (DIAGNOSTIC ONLY).** Build per-patch-edge
neighbour links and classify every patch boundary edge into the three classes
above. Report the census per material and the total edge length weighted by
screen coverage at the review poses. Add a viz that colours patch boundaries by
class. NO behaviour change. Deliverable: the census + the verdict on which of
(A)/(B) dominates.

**S1d-2 — RE-SCOPED by S1d-1's census (see §S1d-1.10).** The stage as written
below targets COPLANAR seams, which the census measures at 31 px of 800 513 and
which `--pom_shell_merge_uv` already handles. The re-scoped order is:
**(a) close the shell at convex ridges and true boundaries (84.9 % of the
population), (b) a per-boundary-class edge policy — discard at TRUE boundaries,
where the geometry behind already fills the pixel correctly and the clamp is the
bug, (c) angled continuation last.** The original text is kept for the record:

**S1d-2 (as originally written) — march continuation across coplanar seams.** Hand-off at domain exit
with the UV transform; bounded hops; terminal action defined and flagged.
Gates: void → single digits AND clamp/smear area → near zero at the review
poses, in BOTH lid and recess modes; grazing depth stays at P1's accuracy with
the cap RAISED (that is the point).

**S1d-3 — side faces + protrusion restored.** Extrude side quads at true
boundaries; re-enable the outward lid. Gates: void vs tessellation; no
interpenetration with neighbouring meshes (the C2 metric); shadow-cube and
mirror deltas measured, with the recess-only arm's **0 of 13.5M texels** as the
standard to beat or justify; see-through in the valleys demonstrated in a crop.

**S1d-4 — the honest comparison.** Tessellation vs closed-shell vs recess-only:
look crops at every review pose, void/smear/offscreen tables, and perf at
MATCHED QUALITY (error vs the converged reference of each arm's own semantics),
not at matched flags. Only then does anyone discuss defaults.

## Mandatory gates on every arm, every stage

These exist because the campaign shipped a recommendation with 143,835 px of
known error in it, and because the converged reference is blind to seam holes
(it shares the open-boundary model — inventory class C7).

1. **void (z==0) vs tessellation**, at every pose in
   `docs/greets_review_poses.txt`. Tessellation scores 0–24. Anything in the
   thousands is a failed arm, whatever else improved.
2. **Clamp/fallback area** — the pixels where the march had no answer, reported
   as a percentage of frame. Holes and smears are the same population; report it
   even when the mode makes it invisible.
3. **Offscreen deltas** — shadow-cube texels + mirror content vs the no-shell
   arm. Recess-only achieves 0/13,533,184.
4. **Error vs a converged reference of the SAME semantics.** Never across
   semantics (P0 §8 documents the rule).
5. **Look at the output images before reporting.** A metric that improves while
   the picture breaks is how this campaign reached this document.

## Discipline (documented failures)

Dummy SDL drivers always, never on screen. Renders sequential + backgrounded
(600 s watchdog). ZSH does not word-split unquoted `$FLAGS` — use arrays, and
grep run logs for `unknown flag` / `requires a value`. Never pipe a build
through `head`/`tail` (SIGPIPEs ninja → stale binary). FeatureFlags.def for all
tunables; new flags default OFF; the user flips defaults. Measured vs inferred
stated explicitly; "I don't know" beats a plausible story. Never call ms-scale
costs "tiny". Gates before each commit: render_gate 3/3, city `37e62845`,
fountain `51fff7cd`. Commit on fog-wt; do not push.

## Current standing arm (what to compare against)

```
--deferred --no-greets_displace --pom_shell --pom_recess_only \
  --parallax_pom_cone --parallax_pom=32 --pom_cone_exact=1 --pom_cone_min_step=1 \
  --pom_march_earlyout --pom_shell_cap=16 \
  --pom_shell_world_amp --pom_shell_world_amp_set=0.18 --pom_normal
```

Zero holes, correct shadows/mirrors, no protrusion, smears at grazing.
Tessellation (`--greets_displace`) is NOT a default and never was — it is
opt-in, and the user has retired it (2026-08-05: "we are not going to use it
anyway, it's way too slow"). The actual shipping default is FLAT POM (no
geometric relief). Tessellation remains a measurement arm only.

---

# S1d-1 — PATCH-BOUNDARY TOPOLOGY AND SEAM CENSUS (diagnostic, no behaviour change)

Added 2026-08-05, branch `fog-wt`. Two new flags, **both default OFF**:
`--pom_seam_census` and `--pom_seam_viz`. No pixel of any existing arm moves —
gates in §S1d-1.9. This section answers the gating question above, and the
answer **overturns this plan's own expectation**, so read §S1d-1.6 before
§S1d-1.7.

## S1d-1.0 Method, and the two traps it had to route around

Topology is built inside `PomShell_Build`, on the AUTHORED mesh, from
**position coincidence** (quantized to 1e-4 world) — never from face indices
(the Piramid chunk split re-bins them) and never from vertex pointers
(`MakeFacesIndependentByAngle` has already duplicated every crease vertex, so
two Vertex objects sit at one position with different normals). The census is
captured *before* `PomShell_Build`'s vertex move for the same reason the
amplitude census is, and it refuses to be trusted otherwise: with a non-zero lid
offset the two copies of a corner vertex move along **different** smooth normals
and coincidence is destroyed. The flag prints a WARNING if `offMax > 0`; every
number below was taken under `--pom_recess_only` (offset exactly 0).

A boundary edge of a patch is an edge no *other face of the same patch* shares.
For each one, every face in the scene sharing that edge position is examined:

| relation to this patch's plane | class | why |
|---|---|---|
| normal dot ≥ 0.999 and \|Δd\| ≤ 1e-3 | **COPLANAR CONTINUATION** | same surface, same plane — a hole here is always wrong |
| folds so the neighbour lies IN FRONT of this plane | **ANGLED-IN** (concave) | the material is a UNION across the seam: a ray leaving laterally is *still inside solid* and must enter the neighbour's shell |
| folds so the neighbour lies BEHIND this plane | **ANGLED-OUT** (convex) | the material is an INTERSECTION: the ray *leaves the solid*. Exit is the geometrically correct answer — this is a true silhouette |
| nothing continues | **TRUE BOUNDARY** | free edge; exit is correct and side faces belong here |

The concave/convex decision is exact, not heuristic: a plane hinged on the
shared edge lies entirely on one side of this patch's plane, so the sign of
`N_patch · centroid_neighbour + d_patch` **is** the fold direction.

Edges with no shared-edge partner get a geometric **outward probe** — the edge
pushed 0.02 / 0.10 / 0.35 world into the plane beyond the patch, tested against
every triangle in the scene — which is how T-junctions and non-edge-adjacent
coplanar abutments (the doorway thresholds) are found. Probe-classified edges
are **subdivided at 0.25 world** and each piece classified separately; at
1 world the t=6097 population was still being mislabelled, because its exits sit
within 0.8 world of a class transition. That correction is recorded because it
changed the verdict at one pose (see §S1d-1.8).

## S1d-1.1 The scene's topology, measured

```
'rooms'  196 faces -> 67 patches over 51 authored planes; 55 plane groups
         patch UV box: short side 0.082 / 0.422 / 3.086 (min/med/max)
                       long  side 0.103 / 1.131 / 5.246
'floor'   30 faces ->  6 patches over  1 authored plane;   1 plane group
         patch UV box: short 0.338 / 0.689 / 1.750, long 0.699 / 1.654 / 2.132
```

Grazing lateral travel at the standing arm (`uvAmp` 0.03 × `--pom_shell_cap=16`)
is **0.48 UV** on the walls. The median wall patch is 0.42 UV across its short
axis. **The median patch is narrower than one grazing ray's travel** — the
premise in "The design" above is confirmed, and it is worse than the 0.4–2.1
range quoted there (the narrowest patch is 0.082 UV).

## S1d-1.2 EDGE CENSUS — by count and by world length

Counts are of classified SEGMENTS (probe-classified edges are subdivided), so
**length is the meaningful column**.

| material | segments | total length | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY |
|---|---|---|---|---|---|---|
| `rooms` | 2 057 | 1 847.73 world | 139.48 (**7.5 %**) | 1 393.30 (**75.4 %**) | 305.07 (**16.5 %**) | 9.88 (**0.5 %**) |
| `floor` | 449 | 441.51 world | 53.81 (**12.2 %**) | 387.70 (**87.8 %**) | 0.00 (0 %) | 0.00 (0 %) |
| **both** | 2 506 | 2 289.24 world | 193.29 (**8.4 %**) | 1 781.00 (**77.8 %**) | 305.07 (**13.3 %**) | 9.88 (**0.4 %**) |

`::mirUV` clones do not appear here: `GreetsFixBitangentHandedness` runs AFTER
`PomShell_Build`, so a clone is a memberwise copy that **shares** the patch
domains, the sibling boxes and `Face::PomShellGroup`. One topology serves both,
and the screen tables below report the clones separately because they own
different pixels.

Two structural facts that fell out and matter later:

- **greets has essentially no free edges.** 9.88 world of 2 289 (0.4 %) is a
  true boundary. The architecture is closed: every wall edge meets the floor,
  the ceiling, another wall or a column.
- **The walls are NOT zero-thickness double-sided sheets.** The patch pairs that
  look coincident in the dump (e.g. `g=22` N=(1,0,0) d=12.344 and `g=23`
  N=(−1,0,0) d=12.344) are two *different* walls at x=∓12.344 — a symmetric
  room, not a sheet. Measured: **0** edges whose only partner was a reverse
  face, in either material.
- **ANGLED-IN neighbours are mostly shelled surfaces but not entirely.**
  `rooms`: 969.24 world of the 1 393.30 has a `rooms`/`floor` neighbour;
  **424.06 world (30 %) has an UNSHELLED neighbour** — the ceiling `siling`
  (43 edges) and the `teleporter`. Continuation into those requires shelling
  them first.

## S1d-1.3 SCREEN WEIGHTING — the number the next stage needs

**Instrument.** Two arms of the standing recipe per review pose, differing in
one flag, with `FDS_DUMP_TXTR=1`:

- `clamp` = the standing arm (`--pom_recess_edge=0`). Its dumped march UV *is
  the geometric UV* exactly on the pixels the march could not answer.
- `nodom` = the same arm plus `--no-pom_shell_domain`. Its dump is the
  **unclamped landed UV**, i.e. where the ray actually crossed the height field
  after leaving the patch.

The pixels where the two dumps differ are exactly the population §10.6 of the
inventory sized by the same differencing — and because `--pom_recess_edge=0`
also catches never-bracketed lanes in BOTH arms, the difference isolates
**domain exits alone**. The pair gives the ray's entry UV and its landed UV, so
the exit point (where the segment leaves the domain = own UV box ∪ sibling
boxes) is solved by bisection and attributed to the nearest classified boundary
segment. Attribution quality, measured: the exit-to-nearest-classified-edge
distance is **0.0000 UV at p50 and p90, 0.0076 at p99** — the exits land on the
classified edges, they are not being snapped to something far away.

**Control (this is what makes the mask trustworthy).** A third arm,
`--pom_recess_edge=2` (discard), renders the same geometry and march. Its void
must be a SUBSET of the clamped mask. Measured over all 13 review poses:
**231 068 void px, of which 231 064 are inside the clamped mask (4 px outside,
0.002 %)**. And the per-pose voids reproduce the inventory's published figures
to the digit: **20 244 at t=5958** (§10.1's grazing-smear number) and
**85 065 at t=5743** (§10.6's `recW18` "if it discarded instead" column).

**One measured caveat on "they are the same pixels."** The proof at the top of
this file — recess-only `edge=0` (0 void) vs `edge=2` (20 244 void), same
geometry, same march — reproduces EXACTLY, and my clamped mask is a strict
superset of that void. But the LID arm is a *different* comparison and does NOT
give the same pixel set: over the 13 poses the lid arm voids 413 100 px of which
only **79 420 (19 %)** are inside the recess arm's clamped mask, and 333 680 lid
voids fall outside it (worst: p5963, 101 518 px; p5958b, 107 012 px). That is
expected — the lid arm moves every wall vertex `amp/2` along its smooth normal
and adds `--pom_shell_base_clip`, so its discard population is not the recess
arm's — but it means "the holes and the smears are the same pixels" is proven
*within one geometry*, not across the lid/recess fork. Recorded because it would
be easy to over-read.

### The census, all 13 review poses

`clamped` = pixels the march could not answer. `void` = the subset that goes
black when the answer is a discard, i.e. the reported gash.

| pose | shell px | clamped | % frame | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY |
|---|---|---|---|---|---|---|---|
| p5743 | 1 719 288 | 94 952 | 4.58 | 2 | 12 346 | **82 604** | 0 |
| p5773 | 1 716 354 | 104 943 | 5.06 | 0 | 12 373 | **92 570** | 0 |
| p5813 | 1 723 713 | 115 466 | 5.57 | 0 | 21 768 | **93 698** | 0 |
| p5843 | 1 716 718 | 39 798 | 1.92 | 0 | 17 469 | 22 329 | 0 |
| p5963 | 1 743 657 | 36 386 | 1.76 | 0 | **20 308** | 16 078 | 0 |
| p6133 (mirror) | 1 993 796 | 8 940 | 0.43 | 20 | **8 920** | 0 | 0 |
| p6293 (mirror) | 2 034 521 | 7 961 | 0.38 | 9 | **7 952** | 0 | 0 |
| p5958a | 1 912 600 | 89 803 | 4.33 | 0 | 4 138 | **85 665** | 0 |
| p5958b | 2 003 920 | 73 557 | 3.55 | 0 | 3 305 | **70 252** | 0 |
| p5958c | 1 744 740 | 51 871 | 2.50 | 0 | 6 911 | **44 960** | 0 |
| p5958d | 1 674 997 | 63 364 | 3.06 | 0 | 5 296 | **58 068** | 0 |
| p6097 | 2 073 600 | 113 244 | 5.46 | 0 | 0 | 17 698 | **95 546** |
| p2845 | 2 073 600 | 228 | 0.01 | 0 | 228 | 0 | 0 |

| population | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY | total |
|---|---|---|---|---|---|
| **all clamped** | **31 (0.004 %)** | 121 014 (15.1 %) | **583 922 (72.9 %)** | 95 546 (11.9 %) | 800 513 |
| **void subset** | **31 (0.013 %)** | 76 765 (33.2 %) | **154 268 (66.8 %)** | 0 (0.0 %) | 231 064 |

### The answer to the question the plan asked

> *of the pixels the march currently cannot answer, what fraction sits at a
> coplanar seam that continuation would fix?*

**0.004 %.** 31 pixels of 800 513, across every pose the user reviews from.
Of the pixels that actually go BLACK, **0.013 %**.

By owning surface:

| material | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY | total |
|---|---|---|---|---|---|
| `rooms` | 0 | 14 167 | 485 287 | 0 | 499 454 |
| `rooms::mirUV` | 2 | 82 502 | 98 635 | 95 546 | 276 685 |
| `floor::mirUV` | 29 | 24 345 | 0 | 0 | 24 374 |
| `floor` | 0 | 0 | 0 | 0 | 0 |

By what is on the FAR side of the seam the ray left through:

| neighbour | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY | total |
|---|---|---|---|---|---|
| `rooms` | 2 | 55 386 | 565 744 | 0 | 621 132 |
| nothing | 0 | 0 | 0 | 95 546 | 95 546 |
| `siling` (ceiling, **not shelled**) | 0 | 25 350 | 18 178 | 0 | 43 528 |
| `floor` | 29 | 39 041 | 0 | 0 | 39 070 |
| `teleporter` | 0 | 1 237 | 0 | 0 | 1 237 |

## S1d-1.4 WHY coplanar is zero: (A) is already shipping, under another name

`--pom_shell_merge_uv` (default **0.05**, landed in `dfb4272`) gives each patch
a SIBLING BOX LIST and the domain test is the UNION of the boxes. On a plane the
UV chart is a world-axis planar projection, so **the UV relationship between two
coplanar patches is the IDENTITY** — measured, worst disagreement across any
coplanar seam **1e-6 UV** on `rooms` and **6.8e-4 UV** on `floor`, over 549 + 217
classified coplanar segments. A coplanar hand-off therefore needs *no transform
at all*, and extending the domain to the union is exactly equivalent to
performing it.

Measured directly: pixels whose landed UV left their patch's OWN box but landed
inside a sibling's — i.e. coplanar continuations the kernel already makes:

| pose | clamped | already carried by siblings | share of the would-be population |
|---|---|---|---|
| p6133 (mirror) | 8 940 | 18 661 | **67.6 %** |
| p6293 (mirror) | 7 961 | 10 338 | **56.5 %** |
| p6097 | 113 244 | 30 465 | 21.2 % |
| p5773 | 104 943 | 4 277 | 3.9 % |
| p5743 | 94 952 | 3 112 | 3.2 % |
| **all 13 poses** | 800 513 | **70 585** | **8.1 %** |

So coplanar continuation is worth 70 585 px scene-wide and up to two thirds of
the population at the mirror poses — **and it is already being done.** What is
left over is, by construction, not coplanar.

## S1d-1.5 VIZ — look at it, do not trust the table

`--pom_seam_viz=1` (default 0) draws every classified boundary segment over the
final frame, depth-tested against the frame's own Z, on the `DisplaceViz`
overlay path: GREEN coplanar, ORANGE angled-in, MAGENTA angled-out, RED true
boundary. Modes 2/3/4 isolate one class. Main view only.

| crop (`docs/img/s1d_seams/`) | what |
|---|---|
| `p5743_A_seamviz_full.png` | the engine viz at the user's primary hole pose (full frame, ½ scale) |
| `p5743_C_classmap_full.png` | every clamped pixel, coloured by the class it exits through |
| `p5743_E_voidclassmap_crop.png` | **the money shot** — only the pixels that go black, at the right-hand wall corner (crop 1480,60–1920,1040) |
| `p5743_F_recdisc_crop.png` | the SAME crop of the discard arm: the black gash itself |
| `p6097_B_seamviz_crop.png` | the corner pose, engine viz (crop 150,60–800,1040) |
| `p6097_C_classmap_full.png` | the corner pose — a wide RED band where the wall genuinely ends |
| `p5958b_C_classmap_full.png` | the grazing-smear pose — MAGENTA at every convex panel corner |
| `p5958b_F_recdisc_crop.png` | the same pose's discard arm (crop 120,60–800,1040) |

`p5743_E` and `p5743_F` are the same crop of the same frame: **the magenta band
and the black gash are the same shape, pixel for pixel.** The user's reported
"full-height black gash on the right wall" is one boundary segment —
`rooms` g=9, world (9.8748, 0..4.9374, −49.3740), a 27° CONVEX ridge against
patch g=10 — and it alone owns **482 171 of the 800 513** clamped pixels across
the review poses.

## S1d-1.6 What the classes MEAN here — the mechanism, stated before the verdict

- **ANGLED-OUT (convex ridge), 72.9 % of clamped / 66.8 % of void.** The
  material is the intersection of the two half-spaces, so a ray that crosses the
  ridge while inside the slab has genuinely *left the solid*. Discard is the
  correct shell-theoretic answer. It voids anyway, because greets models its
  walls as surfaces with no interior: past the ridge there is no geometry to be
  revealed. A CLOSED shell fixes this by construction — the side face at the
  ridge terminates the ray on the shell boundary instead of letting it escape
  into nothing.
- **ANGLED-IN (concave fold), 15.1 % / 33.2 %.** The material is the union, so
  the ray is still inside solid and the neighbouring surface is what it runs
  into. This one genuinely needs a hand-off into the neighbour's chart.
- **TRUE BOUNDARY, 11.9 % of clamped, 0.0 % of void.** Every one of these 95 546
  px is at p6097, where the wall really ends and **something behind legitimately
  wins the pixel** — measured: zero of them void under the discard arm. So at a
  true boundary the *discard is already right*, and it is the recess arm's CLAMP
  that is wrong there (it paints flat wall over a hole you are meant to see
  through). This is the one class where the two failure modes are NOT symmetric.
- **COPLANAR, 0.004 %.** Already handled (§S1d-1.4).

## S1d-1.7 VERDICT

**Cross-patch continuation across COPLANAR seams — the plan's option (A) as
scoped above — would fix 31 pixels of 800 513. It does not dominate. It is
already implemented.**

The plan's expectation ("Expected to be the large majority — patches are split
by mesh topology, not by geometry") is **wrong for greets**, and the reason is
in the code: the union-find already merges every coplanar edge-adjacent face
into ONE patch, and `--pom_shell_merge_uv` already unions the coplanar patches
that do not share an edge. By the time a ray leaves a patch domain, it has left
the *plane*.

What dominates instead, in order:

| rank | class | clamped | void | what fixes it |
|---|---|---|---|---|
| 1 | ANGLED-OUT (convex ridge) | 72.9 % | 66.8 % | **(B) side faces** — close the shell at the ridge |
| 2 | ANGLED-IN (concave fold) | 15.1 % | 33.2 % | **(A′) angled continuation** — hand-off with a chart change |
| 3 | TRUE BOUNDARY | 11.9 % | 0.0 % | **(B) side faces**; discard already correct, the CLAMP is the bug |
| 4 | COPLANAR | 0.004 % | 0.013 % | already shipping |

**(B) dominates: 84.9 % of the pixels the march cannot answer, and 66.8 % of the
pixels that actually go black, sit at a boundary where the correct answer is a
closed shell, not a chart hand-off.** The remaining 15.1 % / 33.2 % needs a
hand-off, but the coplanar kind that is cheap is not the kind that is needed.

## S1d-1.8 FEASIBILITY for S1d-2, measured

**Is the coplanar UV relationship a simple affine transform?** It is the
IDENTITY. Worst disagreement 1e-6 UV (`rooms`) / 6.8e-4 UV (`floor`). Zero
transforms to build. (And zero pixels to win — §S1d-1.4.)

**The angled ones are not simple.** Over 1 700 classified angled segments:

- **27 distinct fold angles**: 18, 21, 25, 26, 27, 28, 29, 32, 33, 37, 38, 41,
  42, 44, 45, 49, 50, 52, 58, 63, 64, 65, 71, 78, 88, 90, 91 degrees.
- **71 distinct (fold, chart-scale-to-0.1, mirror) buckets.** The largest are
  90°: (scale 2.0 mirrored) ×219, (0.4) ×171, (2.5) ×168, (1.0) ×126,
  (1.0 mirrored) ×120, (2.0) ×115.
- Chart scale ratio across a seam spans **0.79 … 2.51** — the wall↔ceiling and
  wall↔floor seams carry a factor of 2–2.5 because the materials' UV densities
  are 6 and ~15 world per tile.
- **710 of 1 700 angled segments (41.8 %) are MIRRORED** — the neighbour's chart
  has the opposite handedness, so a hand-off must flip an axis. This is the same
  handedness split `GreetsFixBitangentHandedness` already knows about; it is not
  a surprise, but it means the transform is not a rotation+scale, it is a full
  2×3 affine including reflection.
- No patch mixes handedness internally (each is coplanar with one chart), so the
  4:1 trapezoid chart that broke the tessellation bake's `edgeAlignedQuad` has
  **no analogue here** — every patch chart is a single affine map. Measured:
  `rooms` per-plane world-per-UV is constant to four decimals (§8.3 finding 1
  of the inventory), and this census found zero degenerate charts.

**Hop budget — how far past the boundary does the ray actually need to go?**
Measured on every attributed pixel (the distance from the exit point to where
the march landed, i.e. how far into the neighbour the hit is):

| quantity | p50 | p90 | p99 | max |
|---|---|---|---|---|
| remaining UV travel after the exit | 0.0217 | 0.1393 | 0.2263 | 8.55 |
| … as a fraction of the patch's own UV width | 0.022 | 0.159 | 0.259 | 9.24 |
| same-plane box crossings on the whole ray | 1 | 1 | 2 | 6 |

At mip 0 (1 024 texels/tile) the median is **22 texels past the boundary** and
p99 is **232 texels**. **One hop resolves the p99 case**; the plan's proposed
budget of 4 is generous, and 2 would do. The ray leaves its patch only just
before it would have hit — it is not travelling across the room.

**What S1d-2 cannot do without more work:** 43 528 px (5.4 % of the clamped
population) exit toward `siling`, the ceiling, which has **no height map and no
shell**. A hand-off has nothing to hand off to there until the ceiling is
shelled.

## S1d-1.9 GATES — every new flag default OFF, every pin byte-exact

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest `4ac809e5`, conetest `b41894f9`, halotest `166fa25a`) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets `shell` cone-8 `t=6097` | `193427ccb28163705ea6baa5500afd0c` — byte-exact |
| greets `tess` `t=6097` | `3f86c73cc7ed8f0ad8f57b12984537d0` — byte-exact |
| greets `flat` `t=6097` | `9d095fbcac0c00888578d56172786997` — byte-exact |
| wasm | `cmake --build build-wasm` links clean (80/80) |
| bad flags | 0 across all 61 run logs (55 snapshot renders + 5 gate runs + the census run) (`unknown flag` / `requires a value`) |

The census is init-time only and the viz is a post-tonemap overlay on the main
view, so neither can reach the rasterizer, the offscreen passes or the G-buffer.
Cost of `--pom_seam_census` when ON: the whole greets init run takes 3.7 s wall.

## S1d-1.10 RECOMMENDATION for S1d-2

Build **side faces first (the plan's B), not march continuation (A)**.

1. **S1d-2a — close the shell at ANGLED-OUT ridges and TRUE boundaries.**
   Together 84.9 % of the clamped population and 66.8 % of the void. At a convex
   ridge the side face is the neighbour's own plane, so it is derivable from the
   topology this stage already builds — no new authoring. Gate it on void at all
   13 review poses and on the `recdisc` arm's void going to single digits.
2. **S1d-2b — fix the clamp at TRUE boundaries independently and cheaply.**
   Those 95 546 px void ZERO under a discard: the geometry behind already fills
   them correctly. A per-boundary-class edge policy (discard at TRUE, clamp
   elsewhere) is a *table lookup on data this stage already produces* and needs
   no new geometry. It is the cheapest measured win available and should be
   costed before anything else.
3. **S1d-2c — angled continuation, last.** 15.1 % / 33.2 %, and it is the
   expensive one: 71 distinct affine transforms, 42 % of them mirrored, and 5.4 %
   of its target pixels point at an unshelled ceiling. One hop suffices
   (p99 = 232 texels), so the kernel cost is bounded; the *bake* is where the
   work is.
4. **Do NOT build coplanar continuation.** It is 31 pixels, and
   `--pom_shell_merge_uv` already does it.

One caveat stated plainly: this census is of GREETS. The verdict "coplanar is
nothing, convex ridges are everything" is a property of this scene's
architecture (closed rooms, symmetric wall pairs, no free edges), not a general
truth about the shell model. The instrument is scene-agnostic — point it at
another material and it will answer again.

---

# S1d-2 — CLOSING THE SHELL: side faces, and a per-boundary-class edge policy

Added 2026-08-05, branch `fog-wt`. Three new flags, **all default OFF, all
byte-null** (gates in §S1d-2.7): `--pom_shell_side_faces`,
`--pom_shell_side_edge`. Built in the order S1d-1's census established, each
measured before the next was started.

Framing correction carried in from the coordinator mid-stage: **tessellation is
NOT the shipping default and never was** (`greets_displace` = 0, nothing enables
it) and the user has retired it as a candidate look. The incumbent is **FLAT
POM**. Tessellation stays in every table below as the void-gate yardstick and as
the reference for what correct relief looks like — not as a rival to beat on
cost.

## S1d-2.0 The mechanism S1d-1 pointed at, stated exactly

At a CONVEX ridge the solid is the **intersection** of the two half-spaces, so
the side face of patch A's slab is patch B's own plane — and that plane **leans
outward with depth**. At depth d below the authored plane the material reaches
`cot(fold)·d` PAST the ridge line. The shell's vertical UV box cuts it off
exactly there, and that cut is where the user's gash (under a discard) and the
grazing smear (under a clamp) both live.

The derivation is exact and needs no trig. The neighbour's half-space test

```
N_b · [ P_uv(u,v) + N_a·(h − h0)·ampWorld ] + d_b  ≤  0
```

is LINEAR in `(u, v, h)`. Along a box side's own axis it reads

```
u ≤ uMax + lean·(h0 − h),   lean = (N_a·N_b)·ampWorld / |N_b · dP/du|
```

with `h0` = the slab height of the authored plane (1 under `--pom_recess_only`,
0.5 under the lid). So the whole closed shell is four leaning half-planes, four
FMAs and one compare group per covered pixel.

Sanity check against the geometry: a 90° fold gives `N_a·N_b = 0` → lean 0 → the
vertical extrusion, which is right for a box corner. The user's 27° ridge gives
`cot 27° × uvAmp` = 1.96 × 0.03 = **0.059 UV**, and the bake independently
measures **0.0431 / 0.0515** for that patch's two sides. A TRUE boundary and a
concave fold both keep lean 0: at a free edge the wall really ends, and at a
concave fold the material is a UNION, so the neighbour's plane does not bound
this shell at all (that is the hand-off case, S1d-2c).

## S1d-2.1 What the bake produces

`PomShell_Build`, under `--pom_shell_side_faces`, reuses S1d-1's
position-coincidence topology (no new authoring, no new flag for it — the flag
arms the capture without arming its printing) and emits, per patch and per box
side (`k` = 0 uMin, 1 uMax, 2 vMin, 3 vMax):

- the **dominant boundary class** by length,
- the **lean**, clamped to `[0, uvAmp × --pom_shell_cap]` (a lean above the ray's
  own lateral rate can never bind),
- the **TRUE-BOUNDARY sub-interval** of that side (see §S1d-2.4 for why a
  dominant class cannot carry that one).

```
[POM-SIDE] 'rooms' mode=1: 67 patches x 4 sides; dominant class
           cop=23 in=178 out=46 true=1 none=20;
           31 sides LEAN (0.00118..0.06000 UV per unit slab height);
           8.7% of classified boundary length sits >2% of the box away from
           every box side (attributed to the nearest one anyway)
[POM-SIDE] 'rooms' 2 of 268 sides carry a TRUE-BOUNDARY sub-interval
[POM-SIDE] 'floor' mode=1: 6 patches x 4 sides; cop=5 in=19 out=0 true=0 none=0;
           0 sides LEAN
```

`floor` gets **no leans at all** — its boundaries are entirely concave or
coplanar — so this whole stage is a `rooms` change. The user's gash ridge is
`rooms` g=9: `uMin=ANGLED_OUT/0.04314 uMax=ANGLED_OUT/0.05153`.

Two approximations, both measured and stated rather than hidden: the segment is
attributed to the box side its UV midpoint is nearest (8.7 % of `rooms` boundary
length sits more than 2 % of the box away from every side — L-shaped patches'
inner corners), and mode 1 takes the dominant class's lean. Mode 2 takes the
MINIMUM lean over every segment on the side, so a side mixing convex with
anything else gets 0. **Measured difference between the two: 685 px of depth over
all 13 review poses** — the mixed-side risk is not real on this scene.

## S1d-2.2 INSTRUMENT — and it reproduces S1d-1 to the digit

Every number below is 1080p, 13 review poses from `docs/greets_review_poses.txt`,
dummy SDL drivers, sequential renders. Void = `z == 0` px from
`FDS_SNAPSHOT_ZDUMP`. Clamped = pixels whose dumped march UV (`FDS_DUMP_TXTR`)
differs between the arm and the same arm plus `--no-pom_shell_domain` — S1d-1's
own instrument.

| quantity | S1d-1 published | this stage measured |
|---|---|---|
| `recdisc` void, p5743 | 85 065 | **85 065** |
| `recdisc` void, p5958b | 20 244 | **20 244** |
| `recdisc` void, 13 poses | 231 068 | 231 073 |
| `rec` clamped, p5743 | 94 952 | **94 952** |
| `rec` clamped, p6097 | 113 244 | **113 244** |
| `rec` clamped, 13 poses | 800 513 | 809 415 |
| `lid` void, 13 poses | 413 100 | **413 100** |
| seam census, `rooms` total length | 1 847.73 world | **1 847.73** |

The two totals that differ do so at three poses (p5773 +26, p5813 +1 435, p5958a
−2 060, p5958b +1 512, p5958c +1 261, p5958d +895). Both runs are mine on the
same binary family and the per-pose voids reproduce exactly, so I do not have an
explanation and record both rather than picking one. Everything below is
internally consistent — same instrument, same session, arm vs arm.

## S1d-2.3 STEP 1 — SIDE FACES (`--pom_shell_side_faces=1`)

### The population the march cannot answer

| pose | clamped `rec` | clamped `+side faces` | Δ |
|---|---|---|---|
| p5743 | 94 952 | **38 485** | **−59.5 %** |
| p5773 | 104 969 | 79 720 | −24.1 % |
| p5813 | 116 901 | 102 380 | −12.4 % |
| p5843 | 40 645 | 36 698 | −9.7 % |
| p5963 | 37 252 | 35 411 | −4.9 % |
| p6133 (mirror) | 8 940 | 8 950 | +0.1 % |
| p6293 (mirror) | 7 961 | 7 967 | +0.1 % |
| p5958a | 91 863 | 78 785 | −14.2 % |
| p5958b | 75 069 | 51 375 | −31.6 % |
| p5958c | 53 132 | 46 267 | −12.9 % |
| p5958d | 64 259 | 30 187 | **−53.0 %** |
| p6097 | 113 244 | 113 258 | +0.0 % |
| p2845 | 228 | 228 | 0 |
| **all 13** | **809 415 (3.0 % of pixels)** | **629 711** | **−22.2 %** |

### The subset that actually goes BLACK — the user's gash

Same geometry, same march, `--pom_recess_edge=2`:

| pose | void `recdisc` | void `+side faces` | Δ |
|---|---|---|---|
| **p5743 (the reported gash)** | **85 065** | **28 634** | **−66.3 %** |
| p5773 | 35 365 | 24 803 | −29.9 % |
| p5813 | 19 030 | 16 747 | −12.0 % |
| p5843 | 11 661 | 11 337 | −2.8 % |
| p5963 | 13 343 | 13 250 | −0.7 % |
| p6133 (mirror) | 7 324 | 7 334 | +0.1 % |
| p6293 (mirror) | 7 080 | 7 086 | +0.1 % |
| p5958a | 9 470 | 7 326 | −22.6 % |
| **p5958b (the reported smear)** | **20 244** | **4 115** | **−79.7 %** |
| p5958c | 5 784 | 5 137 | −11.2 % |
| p5958d | 16 479 | 3 581 | −78.3 % |
| p6097 | 0 | 1 | — |
| p2845 | 228 | 228 | 0 |
| **all 13** | **231 073** | **129 579** | **−43.9 %** |

### The mandatory gates

| gate | tess | flat POM (incumbent) | `rec` | `rec + side faces` |
|---|---|---|---|---|
| **void, 13 poses** | 13 | 5 | 5 | **5** |
| clamp/fallback, % of frame | n/a | n/a | 0.01–5.6 % | **0.01–5.5 %** |
| offscreen: vertices moved | yes | none | **none** | **none** |

Offscreen deltas are **zero by construction and not by tuning**: this stage adds
no geometry and moves no vertex — `--pom_recess_only` still builds the shell
without the lid offset, so the shadow cube, the mirror RTT and the env probes see
the authored wall exactly as P2-A measured (0 of 13 533 184 shadow texels). The
kernel change is confined to the G-buffer fill's domain test. The mirror-pose
numbers above (p6133/p6293, which render through the mirror RTT) move by 6–10 px,
i.e. the mirror content is essentially unchanged.

### LOOK — and this is where step 1 is not a clean win

`docs/img/s1d_side/`.

- **`p5743_A_gash_recdisc_sidedisc_tess.png`** (the discard arm, so the defect is
  visible): the baseline's full-height black gash becomes a **thin black sliver**.
  Narrowed, not closed. That is the 85 065 → 28 634 in a picture.
- **`p5958b_D_joint_rec_side_tess.png`**: the panel-seam mortar joint **tightens
  and regains structure**; the baseline's wide washed-out band is gone and the
  joint reads closer to `tess`. At this pose the arm also stops painting flat
  wall over a background element the tessellation arm shows — it agrees with the
  reference where the clamp did not. **This one is a straight improvement.**
- **`p5743_B_corner_rec_side_tess.png` — the problem.** At the user's primary
  pose the recovered band renders as a **saturated rust/brown vertical stripe**
  that is not in `tess` and not in the baseline (which shows a soft flat band
  there). I looked at it at 1.6× and at full frame: at full frame it is a warm
  vertical stripe near the right edge, noticeable but not glaring; at zoom it is
  plainly an artefact.

**Mechanism of the stripe, stated plainly.** The lean is geometrically correct —
the material really is there — but the *content* the march finds there is patch
A's chart EXTRAPOLATED past the ridge, and the true content belongs to patch B.
The domain now reaches up to 0.06 UV beyond the ridge, which is **61 texels at
mip 0** and **0.36 world**. So step 1 buys the right shell shape and pays for it
with the wrong texture in the band it recovers. **That gap is exactly S1d-2c (the
angled hand-off), and step 1 makes the case for it stronger, not weaker.**

`--pom_shell_side_edge=2` (land the ray ON the side face — its own crossing of
the leaning plane, so a real face at a real depth instead of the flat wall) makes
this **worse**: 623 015 px of depth change over the 13 poses, and at p5743 the
stripe widens and acquires a mirrored repeat
(`p5743_C_sideedge2_rec_side_e2_tess.png`). Same root cause, more of it. Kept as
a flag value because it is the honest implementation of "exit through a real
face", and it is the arm a hand-off would replace.

## S1d-2.4 STEP 2 — PER-BOUNDARY-CLASS EDGE POLICY (`--pom_shell_side_edge=1`)

**First attempt: 0 pixels changed, at every pose.** A per-box-side DOMINANT class
cannot express a TRUE boundary in greets. Measured: TRUE boundary is **9.875 of
1 847.73 world** of `rooms` patch boundary (0.5 %) yet owns **11.9 %** of the
pixels the march cannot answer. It is always a minority of whatever box side it
lands on — `rooms` g=2 carries `true=20/4.937` against `in=71/35.796` and
`cop=55/13.578` on the same patch — so the lookup never fired. (That first run
also exposed a print bug: `SeamClassName`'s `default:` returned
`"TRUE_BOUNDARY"`, so the 20 *unattributed* sides were printed as free edges.
Fixed; the enum now prints `NONE`.)

**Second attempt: key on the sub-interval, not the class.** The bake now stores,
per side, the along-side UV span the free edge covers (`v` for a u side, `u` for
a v side), 2 floats and one compare in the kernel. In `rooms` exactly **2 of 268
sides** carry one — patches g=2 and g=12, 4.937 world each, i.e. one doorway jamb
of the room's own height.

| | void 13 poses | px changed vs `rec + side faces` |
|---|---|---|
| `--pom_shell_side_edge=1` | **5** (unchanged) | **100 570, all at p6097** |

**This is the cheapest measured win in the stage.** 100 570 px — 4.85 % of the
frame at p6097 — stop showing flat clamped wall and show the surface that is
really behind, **at zero void cost**, exactly as S1d-1 predicted from "those
95 546 px void ZERO under a discard".

**LOOK: `p6097_E_trueboundary_rec_e1_tess.png`** (full frame, rec | side_edge=1 |
tess). The baseline truncates the near stone block with a **hard straight
vertical cut** and hides the floor behind it. With the policy on, the block
carries down and right and the floor with its grout lines comes back — and that
is what `tess` shows too. **The corner silhouette moves toward the reference.**
Not identical to it, but the direction is unambiguous and it is the one change in
this stage whose picture and whose metric agree.

## S1d-2.5 PROTRUSION — NOT RESTORED. The numbers, and why.

The task's premise was that side faces make outward displacement legal again.
**On the measurement, they do not — they make the lid arm worse.** All arms at
0.18 world amplitude, same march, 13 review poses:

| arm | void, 13 poses | p5743 | p5958b |
|---|---|---|---|
| `rec` (recess-only, the standing arm) | **5** | 0 | 0 |
| `rec + side faces` | **5** | 0 | 0 |
| `lid` (protrusion, `--pom_shell_base_clip` on) | 413 100 | 92 306 | 109 815 |
| `lid --no-pom_shell_base_clip` | 368 575 | — | — |
| **`lid + side faces`** (clip kept) | **468 868** | 90 791 | **164 551** |
| **`lid + side faces`** (clip replaced by the side planes) | **933 535** | 97 422 | **573 253 (27.6 % of the frame)** |

**Why, mechanically.** The same leaning half-plane that WIDENS the shell below
the authored plane NARROWS it above — correctly, because at a convex ridge the
intersection solid does converge toward the ridge line as you rise. But my side
faces are only a **domain test**: a ray that hits the lid outside the narrowed
shell is **killed**, when the geometrically correct thing is for it to **enter
the shell lower down through the side face and march from there**. A closed shell
needs side-face ENTRY as well as side-face exit, and entry means a per-lane march
START height — a kernel restructure of the march loop, not another test after it.
That is the concrete next increment, and it is why the flag deliberately does not
force `--pom_shell_base_clip` off: the two are an A/B, not a stack.

**So the see-through-in-the-mortar-valleys demonstration the task asked for is
not delivered by this stage, and I am not going to show a crop of a 413 k-void
arm and call it protrusion.** Recess-only still cannot let stone stand proud;
that remains the single thing flat POM structurally cannot do either, and the
strongest argument for finishing the closed shell properly.

## S1d-2.6 WHERE THIS LEAVES THE THREE ACCEPTANCE TESTS

| the user's complaint | status | evidence |
|---|---|---|
| t=5743 full-height gash | **reduced, not gone.** 85 065 → 28 634 void px (−66 %); a wide band becomes a thin sliver. The recess arm never showed it at all (it clamps), and the clamp band there is now 59 % smaller — but it renders as a rust stripe, which is a new artefact | `p5743_A`, `p5743_B` |
| t=5958 grazing smear | **improved.** clamped 75 069 → 51 375 (−32 %); would-be-black 20 244 → 4 115 (−80 %); the mortar joint tightens toward `tess` and stops covering background the reference shows | `p5958b_D` |
| see-through in the mortar valleys | **NOT delivered.** Protrusion is not made legal by this stage; see §S1d-2.5 | — |

## S1d-2.7 GATES — all three flags default OFF, flag-off byte-exact

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest `4ac809e5`, conetest `b41894f9`, halotest `166fa25a`) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets recess arm, **all 13 review poses**, depth (`z16`) md5 | byte-identical to the pre-change binary |
| wasm | `cmake --build build-wasm` links clean |
| bad flags | **0** across all 299 snapshot run logs (`unknown flag` / `requires a value`) |

Greets COLOUR is not a usable byte gate: the known kernel nondeterminism flips it
about 1 run in 3 (measured here: 3 identical-recipe runs gave 2 identical colour
hashes and 1 different, with the **depth identical 3/3**). Every byte claim above
is on depth, which is deterministic.

**One process failure worth recording so nobody repeats it.** `DEMO/CMakeLists.txt`
has a POST_BUILD rule that copies the freshly-linked binary into `Runtime/DEMO`
on every `cmake --build build`. I rebuilt while a render batch was running and
got an arm that differed from its own baseline at all 13 poses — a "regression"
that was entirely my own binary swap. **Never build while a render batch is in
flight.**

## S1d-2.8 WHAT I DID NOT BUILD, AND THE HONEST NEXT STEP

- **S1d-2c angled continuation** — not started. Step 1's rust stripe is now a
  *measured* argument for it rather than a projected one: the closed shell puts
  the ray in the right place and then samples the wrong chart there.
- **Side-face ENTRY** — the missing half of the closed shell, and the thing
  protrusion actually needs (§S1d-2.5). It is a march-loop change (per-lane start
  height), bounded and well-defined, and it should come before the hand-off:
  without it the lid arm cannot be gated at all, and with it the hand-off has a
  correct volume to hand off inside of.
- **Cost** — not measured this stage. The kernel additions are 4 FMAs + one
  compare group per covered pixel for the side faces and one compare per
  free-edge side for the policy; that is a *hypothesis about the cost*, not a
  measurement, and single-digit ms are not dismissible here. It must be benched
  against flat POM (the incumbent) before anyone discusses defaults.

---

# S1d-2d — SIDE-FACE ENTRY, and the measurement that reframes the lid arm

Added 2026-08-05, branch `fog-wt`. Four new flags, **all default 0/OFF and all
byte-null** (gates §S1d-2d.9): `--pom_shell_side_entry`, `--pom_shell_weld`,
`--pom_shell_lid_edge`, plus mode **3** of the existing `--pom_shell_side_faces`.

The task was: build the march-loop restructure (a per-lane start height) that
S1d-2 identified as the reason side faces made the LID arm worse, and use it to
restore protrusion. **The restructure is built and it works. It is not what was
blocking protrusion.** The measurement below is the actual finding of the stage,
and it is worth more than the restructure.

## S1d-2d.0 CHEAPEST DISCRIMINATOR FIRST — where the lid arm's 413 100 px of void really comes from

Before writing kernel code I decomposed the lid arm's void with flags that
already existed. All numbers: 1080p, the 13 poses of `docs/greets_review_poses.txt`,
dummy SDL drivers, void = `z == 0` from `FDS_SNAPSHOT_ZDUMP`.

| arm (standing lid recipe, amp 0.18 world) | void, 13 poses |
|---|---|
| `lid` (as S1d-2 measured it) | **413 100** |
| `lid --no-pom_shell_domain` | 406 940 |
| `lid --no-pom_shell_base_clip` | 368 575 |
| `lid` with **both** off | **228 411** |
| the same **plus `--no-parallax`** (no march at all) | **198 704** (4 poses: 198 131) |
| the same plus **`--pom_shell_lid_probe`** (lid offset forced to 0) | **0** |

and against the offset itself, geometry only (4 poses):

| lid offset (world) | 0.02 | 0.06 | 0.18 | 0.36 |
|---|---|---|---|---|
| void | 19 416 | 58 665 | 198 131 | 383 364 |

**Linear in the offset, zero at offset zero, unchanged when the march is
disabled entirely.** The lid arm's void is not the march, not the lateral-exit
test and not the base clip: it is a **slit in the geometry whose width is the
lid offset**. `--pom_shell_pin` does not help (0 px changed, and the census
reports `0 pinned` — it keys on vertex SHARING inside one `TriMesh`, and greets'
`rooms` owns 588 verts over 196 faces, exactly 3 per face, so nothing is ever
shared).

Look: `docs/img/s1d_entry/p5963_A_lidtear_lid_weld_flat.png`, left panel — the
corridor wall at t=5963 is torn wide open and you see the next room through it.

**This answers the task's second design question with a measurement rather than
a guess: the lid does NOT raster the pixels a side entry would run on. There is
no fragment there at all.**

## S1d-2d.1 THE WELD (`--pom_shell_weld`, default 0)

`PomShell_Build` moves each vertex along ITS OWN `Vertex::N`.
`MakeFacesIndependentByAngle` has already split the mesh completely, so the two
copies of a corner move along two different normals and the lid opens a wedge at
every convex ridge. Measured on greets `rooms`: **155 distinct vertex POSITIONS
carry the 588 vertex uses, 153 of them with 2+ copies, and 420 uses disagree with
their position's mean normal by more than 1° — worst 78.7°.**

The fix is what shell maps do: extrude along a normal SHARED by every copy of the
position, so adjacent prisms keep a common side and the offset surface stays
watertight — a mitred corner instead of a wedge. `Vertex::N` is untouched
(shading unchanged); only the offset DIRECTION is welded. `Vertex::ShellH`
already models the consequence and picks it up automatically: corner verts go
71 → 438 and `ShellH` min 0.955 → 0.598, i.e. the march now enters at the true
geometric height of the mitre.

| arm | void, 13 poses |
|---|---|
| `lid` | 413 100 |
| `lid --pom_shell_weld=1` | **214 650** |
| `lid`, domain + base clip off | 228 411 |
| the same **+ weld** | **14 163** (with the march disabled: 13 986) |

`--pom_shell_weld=2` also PINS every position a non-target face shares (the
wall/ceiling and wall/floor junctions the per-material weld cannot close). It
halves the residue again (14 163 → 7 701) **but it is not usable**: on greets it
pins 289 of 588 `rooms` verts and every one of `floor`'s 90, so the floor gets no
shell at all (`[POM-SHELL] 'floor': nothing built`). Kept as the diagnostic that
attributes the residue, not as an arm.

## S1d-2d.2 SIDE-FACE ENTRY (`--pom_shell_side_entry`, default 0) — built, correct, and NOT the answer

The restructure the task specified. Both the ray and all four leaning side planes
are AFFINE in the slab height `h`, so the shell over a patch is a convex
polyhedron in `(u,v,h)` and the ray/shell intersection is one slab clip: side `k`
requires `a_k + b_k·h ≤ 0`; `b > 0` bounds the entry height from above, `b < 0`
from below. The march then STARTS at that entry height with the UV at that
crossing instead of at the lid, and a ray whose interval is EMPTY misses the
closed shell entirely — a true silhouette, and the lane is killed.

**Nothing serialises.** `hStart` was already a `Vec8f` (`hEnter` is the per-pixel
interpolated `ShellH`), and every march's `curU = baseU + dU·(hStart − hEnter)`
and `stepH = hStart·1/N` are already per-lane, so a per-lane start height costs
the clip and not one scalar branch. No fallback path was needed. Cost: 4 divides
+ ~16 FMAs + 8 selects per covered shell pixel, only when the flag is on.

**Depth is untouched by construction.** The S1a write is
`Δz = (hitH − hEnter)·A·Vz/(V·N)` — relative to the RASTERED surface. Entry moves
where the march BEGINS, not where the fragment is, so the convention and the Z
continuity across the side face are exactly as before. No change was required.

**Recess mode: inert, and provably so.** Under `--pom_recess_only`,
`hEnter == h0 == 1`, so the entry test reduces to the plain UV-box test the
pixel's own interpolated UV passes by construction. Measured: recess-only with
`--pom_shell_weld=2 --pom_shell_side_entry=1 --pom_shell_lid_edge=1` is
**byte-identical in depth to the plain recess arm at all 13 poses**.

**What it buys on the lid arm, measured (all welded, base clip off):**

| arm | void, 13 poses |
|---|---|
| weld + `side_faces=1` (S1d-2's narrowing side planes) | 269 408 (with base clip) |
| weld + `side_faces=1` + **entry** | 151 103 |
| weld + `side_faces=3` (no narrowing above `h0`) | 166 523 |
| weld only | 169 516 |

So entry does recover most of what mode-1 side faces cost the lid arm
(269 408 → 151 103 in the comparable no-clip pair below), and it is the best of
the three side-face variants. **But it is 10× away from the weld's own floor
(14 163), because the thing it fixes was never the dominant term.**

Worse, once the two remaining terms below are fixed, entry becomes a net LOSS:

| arm (welded, base clip off, `--pom_shell_lid_edge=1`) | void, 13 poses |
|---|---|
| `side_faces=3` (no narrowing) | **14 163** |
| `side_faces=1` (narrowing) | **14 163** |
| no side faces at all | **14 163** |
| `side_faces=1` + **entry** | **81 979** |

Entry's own kill — "this ray is never inside the closed shell" — costs
**67 816 px of pure black**, because mode 1's lean NARROWS the shell above the
authored plane and rejects lid rays that have real material under them. Which is
the next finding.

## S1d-2d.3 `--pom_shell_side_faces=3` — the lean must not narrow the shell above the authored plane

S1d-2a derived the side plane from the neighbour's AUTHORED plane. Under the LID
that is too aggressive: above `h0` the neighbour's own SHELL bounds this one, not
its authored surface, and **with the weld the two lids already MEET at the ridge**
(that is what welding does). The correct side face above `h0` is therefore the
plain box, and the lean applies only below it: `dh = max(0, h0 − h)`.

Mode 3 makes the side faces PURELY ADDITIVE under the lid as well as under
recess — they can rescue pixels the box killed and can never kill one it kept.
It also makes ENTRY inert by construction (the domain at `hEnter ≥ h0` is the
plain box the pixel is inside anyway), so the two are alternatives, not a stack,
and the kernel forces entry off in mode 3.

Under `--pom_recess_only`, `h0 = 1` and `h ≤ 1`, so `max(0, h0−h)` is a no-op:
measured, `--pom_shell_side_faces=1` and `=3` are **byte-identical in depth at
all 13 poses** in the recess arm. Mode 3 is a lid-arm correction only.

## S1d-2d.4 `--pom_shell_lid_edge` — the lid arm needed the recess arm's clamp

With the weld in and the narrowing removed, the last big term is the LATERAL-EXIT
DISCARD. Measured on the welded arm with the base clip off: the exit kill alone
owns **~152 000 px** of the remaining void. That is literally the defect
`--pom_recess_edge=0` removed from the recess arm — a hole punched through solid
wall at an INTERNAL seam where the wall demonstrably continues.

The lid keeps it a discard only because the lid can cover screen the authored
wall does not — and that population is already identified separately, by the base
clip and by entry's "never in the shell" test. So the policy splits:

| failure | action | why |
|---|---|---|
| ray marched the WHOLE slab, hit nothing | **DISCARD** | this is the see-through, the whole point of the lid model |
| side-entry miss (never inside the shell) | **DISCARD** | true silhouette |
| base-clip overhang | **DISCARD** | the lid covers screen the wall does not |
| **lateral exit** | **CLAMP to flat** | real wall under the pixel; the march simply could not follow the relief into a neighbour it cannot address |

## S1d-2d.5 THE ARM, AND EVERY GATE

```
--deferred --no-greets_displace --pom_shell \
  --parallax_pom_cone --parallax_pom=32 --pom_cone_exact=1 --pom_cone_min_step=1 \
  --pom_march_earlyout --pom_shell_cap=16 \
  --pom_shell_world_amp --pom_shell_world_amp_set=0.18 --pom_normal \
  --pom_shell_weld=1 --pom_shell_side_faces=3 --pom_shell_lid_edge=1 \
  --no-pom_shell_base_clip
```

### Gate 1 — void vs tessellation, per pose

| pose | tess | flat POM | recess (standing) | **lid (was)** | **lid (this arm)** |
|---|---|---|---|---|---|
| p5743 | 3 | 0 | 0 | 92 306 | **153** |
| p5773 | 0 | 0 | 0 | 20 954 | **159** |
| p5813 | 1 | 0 | 0 | 10 413 | **0** |
| p5843 | 0 | 0 | 0 | 26 598 | **1** |
| p5963 | 0 | 0 | 0 | 102 013 | **8 700** |
| p6133 (mirror) | 4 | 0 | 0 | 14 285 | **3 509** |
| p6293 (mirror) | 3 | 5 | 5 | 5 383 | **1 641** |
| p5958a | 1 | 0 | 0 | 5 520 | **0** |
| p5958b | 0 | 0 | 0 | 109 815 | **0** |
| p5958c | 1 | 0 | 0 | 1 046 | **0** |
| p5958d | 0 | 0 | 0 | 24 767 | **0** |
| p6097 | 0 | 0 | 0 | 0 | **0** |
| p2845 | 0 | 0 | 0 | 0 | **0** |
| **all 13** | **13** | **5** | **5** | **413 100** | **14 163** |

**−96.6 %.** It is not the recess arm's league (5) and I am not going to pretend
it is: the residue is 13 986 of 14 163 geometric (it survives `--no-parallax`),
i.e. the cross-material junction slit the per-material weld cannot close, and
`--pom_shell_weld=2` shows it can be halved only by destroying the floor's shell.
Nine of thirteen poses are at 0–159.

### Gate 2 — clamp/fallback area, % of frame

Pixels whose dumped march UV (`FDS_DUMP_TXTR`) differs between the arm and the
same arm plus `--no-pom_shell_domain` — S1d-1's own instrument.

| pose | p5743 | p5773 | p5813 | p5843 | p5963 | p6133 | p6293 | p5958a | p5958b | p5958c | p5958d | p6097 | p2845 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fallback % of frame | 4.08 | 4.88 | 4.91 | 2.45 | 1.59 | 0.25 | 0.29 | 3.75 | 3.28 | 2.07 | 2.72 | 4.21 | 0.07 |

716 141 px over the 13 poses = **2.66 % of pixels**, against the recess arm's
809 415 (3.0 %). The population did not go away; under this arm it clamps
instead of voiding.

### Gate 3 — offscreen deltas. THIS ARM DOES NOT FIX THEM.

Shadow cube, `--dump_shadowmap` from `build-dev` (`-DFDS_DEV=ON`, which does not
overwrite `Runtime/DEMO`), t=5743, 76 cube faces, 13 533 184 depth texels:

| pair | texels differing | >8/255 |
|---|---|---|
| `flat` vs **`rec`** | **0 (0.00 %)** | 0 |
| `flat` vs `tess` | 720 250 (5.32 %) | 81 809 |
| `flat` vs `lid` | 2 798 939 (20.68 %) | 39 526 |
| `flat` vs **this arm** | 2 540 585 (**18.77 %**) | 36 509 |

The two control rows reproduce the inventory's published figures (rec 0, tess
720 250 / 5.32 %) to the digit, which is what makes the other two trustworthy.

Final image on NON-STONE pixels only (`FDS_SNAPSHOT_GBUFDUMP` material plane;
the wall's own shading excluded, so what is left came through an offscreen
consumer or a Z-ownership change), colour max-channel delta vs `flat`:

| pair | pose | non-stone px | >4/255 | >12/255 | >32/255 | worst surfaces |
|---|---|---|---|---|---|---|
| `flat`→`rec` | p5743 | 354 312 | 4 179 | **1 411** | 242 | cockpit`::mirUV` 1 150 |
| `flat`→`tess` | p5743 | 354 312 | 3 862 | **1 035** | 101 | cockpit`::mirUV` 680 |
| `flat`→`lid` | p5743 | 354 312 | 16 731 | **9 605** | 4 044 | `siling` 7 907 |
| `flat`→**this arm** | p5743 | 354 312 | 15 720 | **9 033** | 4 346 | `siling` 5 730, `amudim` 1 512 |
| `flat`→`rec` | p6133 (mirror) | 79 793 | 254 | **107** | 28 | cockpit`::mirUV` 91 |
| `flat`→`lid` | p6133 | 79 793 | 4 330 | **3 495** | 2 856 | `siling` 1 939, robot legs 1 157 |
| `flat`→**this arm** | p6133 | 79 793 | 5 069 | **4 243** | 3 567 | `siling` 2 674, legs 1 157 |
| `flat`→`rec` | p6293 (mirror) | 39 070 | 487 | **144** | 19 | cockpit`::mirUV` 128 |
| `flat`→**this arm** | p6293 | 39 070 | 2 332 | **1 550** | 828 | `siling` 810, legs 480 |

**Stated plainly: moving vertices is the lid model's intrinsic cost and none of
this stage's work removes it.** The weld reduces the shadow-cube contamination by
9 % (20.68 → 18.77 %) and leaves the non-stone contamination essentially where it
was (9 605 → 9 033 px >12/255 at p5743, and slightly WORSE at both mirror poses).
Recess-only's 0 shadow texels remains the standard, and this arm does not meet
it. Tessellation, at 5.32 %, is still 3.5× cleaner offscreen than any lid arm.

### Gate 4 — error vs a converged reference of the same semantics

Not run for this arm, and I am saying so rather than substituting something else.
The converged reference (`--pom_ref_march`) shares the OPEN-boundary model
(inventory class C7) — it clamps or discards at exactly the seams this stage
changes — so it cannot arbitrate the change, and building a closed-shell
reference is the same work as the arm. What I do have is the arm-vs-arm
differencing above and the LOOK below.

### Gate 5 — LOOK. What I actually see, before any number.

`docs/img/s1d_entry/`.

- **`p5963_A_lidtear_lid_weld_flat.png`** (`lid` | this arm | `flat`). The old lid
  arm's corridor wall is **torn wide open** — you see the next room through it and
  a black void beneath. This arm's wall is continuous, with one **thin dark slit**
  (~40 px wide against the old ~320 px) where the wall meets the room behind. That
  slit is the 8 700 px residue at this pose, and it is visible if you look for it.
- **`p5743_B_ruststripe_rec_side1_e2_new_tess.png`** (`rec` | `rec+side_faces=1` |
  `+side_edge=2` | this arm | `tess`). This is the answer to the rust-stripe
  question and it is unambiguous: `rec+side_faces=1` shows the **saturated rust
  vertical stripe** S1d-2 reported; `side_edge=2` makes it **wider and darker**;
  **this arm has no stripe at all** and reads like `rec` and `tess`.
- **`p5743_C_wall_flat_rec_new_tess.png`** — the review pose's right wall,
  `flat` | `rec` | this arm | `tess`. No gash, no stripe; this arm sits between
  `rec` and `tess`. `tess` still carries a bevelled ledge at the top-right that
  no per-pixel arm reproduces.
- **`p6097_D_corner_flat_rec_new_tess.png`** — the corner pose. This arm's block
  edge carries a **ragged, relief-shaped shadow boundary** where `flat`'s is a
  smooth polygon edge. Nearest to `tess` of the three, but not identical to it.
- **`p5963_E/F_lidvoid_mask_before/after.png`** — the void mask (magenta) at
  t=5963, before and after.

## S1d-2d.6 PROTRUSION — restored as geometry, NOT demonstrable as see-through

**Restored:** the lid arm is renderable again. Its void is 14 163 instead of
413 100, the black gashes and the mirror bar are gone, and the arm reads clean
next to `flat` and `tess` at every review pose. Stone genuinely stands proud —
measured against flat POM's depth, this arm writes NEARER depth on 16 534 px at
p5743, 42 237 at p5773, 47 466 at p5958b and 175 989 at p6097.

**Not demonstrated:** see-through in the mortar valleys. I looked for it with an
instrument rather than by eye — pixels where a surface more than 3 world units
BEHIND the wall wins, which no relief depth write can produce:

| pose | p5743 | p5773 | p5813 | p5843 | p5963 | p6133 | p6293 | p5958a-d | p6097 | p2845 |
|---|---|---|---|---|---|---|---|---|---|---|
| see-through px | 23 | 17 | 0 | 1 | 24 737 | 0 | 0 | ≤2 | 0 | 0 |

and p5963's 24 737 is the residual geometry slit, not relief. At 0.6 world
amplitude (3.3× the standing arm, where the arm still holds: void 494 at p5743
and 0 at p2845/p5958b/p6097) it is 503 px at p5743 and 0 elsewhere.

**Why, and I believe this is structural rather than a tuning failure: greets is a
closed room.** Behind every shelled wall is another wall at zero distance or
nothing at all, so a mortar valley has nothing to reveal. See-through needs a
relief silhouette against a background, and the 13 review poses do not contain
one on a shelled surface. **So the strongest argument for the lid model is the
one thing this scene cannot show me, and I am not going to manufacture a crop
that suggests otherwise.**

## S1d-2d.7 THE CONCAVE-FOLD HYPOTHESIS (does Z competition make S1d-3 unnecessary?)

Tested against S1d-1's own per-pixel class maps, no new renders needed. For the
recess arm's clamped population, the fraction that VOIDS under
`--pom_recess_edge=2` is the fraction NO other fragment covers:

| class | clamped px | void under a discard | covered by other geometry |
|---|---|---|---|
| COPLANAR | 31 | 31 (100 %) | 0 |
| **ANGLED_IN (concave fold)** | **121 014** | **76 765 (63.4 %)** | 44 249 (36.6 %) |
| ANGLED_OUT (convex ridge) | 583 922 | 154 268 (26.4 %) | 429 654 (73.6 %) |
| TRUE BOUNDARY | 95 546 | 0 (0 %) | 95 546 (100 %) |

**The hypothesis fails for the majority of concave folds in the RECESS arm:
63.4 % of concave-fold pixels have no second fragment to win them on depth.** The
mechanism is geometric — in recess mode the geometry is the authored wall, and at
an inside corner two walls ABUT on screen rather than overlapping, so a pixel on
A's polygon is simply not on B's. There is nothing to arbitrate.

The hypothesis is *structurally* more plausible under the LID, where A's and B's
lids do interpenetrate at a concave fold — but that is exactly the overlap the
weld removes, and this arm reaches 14 163 void with the concave population
CLAMPED (`--pom_shell_lid_edge=1`), not handed off. So on this scene the concave
15.1 % is currently paid for by a clamp, not by a hole.

On the falsification test for the rust stripe: `--pom_shell_side_edge=2` changes
624 180 px over the 13 poses, of which **403 586 are ANGLED_OUT, 119 995
ANGLED_IN and 95 546 TRUE BOUNDARY**. The extrapolation that makes the stripe can
only happen where the lean is non-zero, and the bake gives a non-zero lean ONLY
to ANGLED_OUT sides — at a concave-dominant side the lean is 0, so `side_edge=2`
lands exactly ON the box boundary (the ray's last in-domain point), not past it.
So the stripe is a convex-ridge artefact by construction, and the crop confirms
it: `p5743_B` shows the stripe on the convex ridge and nowhere else.

**Does the stripe survive entry?** Entry cannot touch it: entry is inert in the
recess arm by construction. What removes it is the LID arm's clamp terminal
action plus the halved reach — under the lid `h0 = 0.5`, so the shell extends at
most `lean·0.5 = 0.030 UV` (31 texels at mip 0) past the ridge against the recess
arm's `lean·1.0 = 0.060 UV` (61 texels). **So S1d-3's angled hand-off is still
needed IF the recess arm keeps `--pom_shell_side_faces` on; it is not needed by
the lid arm as configured here.** That is a real fork in the plan and the user
should decide it, not me.

## S1d-2d.8 PERF — measured under load, and I could not resolve it better

Interleaved, `--bench=scene@scene=greets,t=5743,iters=30..40`, 1920×1080, dummy
drivers. **The machine carried load average 5.3 → 15.5 from concurrent sessions
throughout**, and the within-arm spread (59–74 ms over 10 rounds of the same
recipe) is larger than any difference between arms.

Least-contended round (load 5.3), min of 5, ms/frame:

| flat POM | recess | lid | **this arm** | lid+side_faces=1+entry | tess |
|---|---|---|---|---|---|
| 56.2 | 59.1 | 60.0 | **60.1** | 60.3 | 94.9 |

Min of 10 at load 12–13, lid vs this arm vs the entry arm: **59.09 / 59.64 /
60.16**.

So the marginal cost of `--pom_shell_weld` + `side_faces=3` + `lid_edge=1` over
the plain lid arm is **≤ ~0.5 ms/frame** and of the entry arm **≤ ~1.1 ms**, as
an UPPER BOUND from minimum-of-N under contention. I could not measure it to
better than that, and I am not calling it small: at 60 ms/frame, 1 ms is 1.7 %
and the whole shell family is already +4 ms over flat POM, which is the
incumbent. The weld itself is init-time only. A clean bench on an idle machine is
outstanding work.

## S1d-2d.9 GATES — every new flag default OFF, flag-off byte-exact

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest `4ac809e5`, conetest `b41894f9`, halotest `166fa25a`) |
| city `t=1961` (`FDS_CITY_ENV_PIXEL=1`) | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets **recess** arm, 13 review poses, depth **and colour** md5 | **byte-identical** to a binary built from the PARENT COMMIT in a clean worktree, run from the same `Runtime/` |
| greets **lid** arm, 13 review poses, depth **and colour** md5 | **byte-identical**, same method |
| recess + `weld=2` + `side_entry=1` + `lid_edge=1`, 13 poses | **byte-identical** to plain recess (all three inert there) |
| recess `side_faces=1` vs `=3`, 13 poses | **byte-identical** (mode 3 is a lid-only correction) |
| wasm | `cmake --build build-wasm` links clean (82/82) |
| bad flags | **0** across 578 snapshot run logs + the shadow-dump runs |

**Greets colour became a usable byte gate DURING this stage.** A concurrent
session landed `f4e81e9` ("AO maps are 8-bit — reading them as dwords was the
greets nondeterminism") at 04:51, and after it the same recipe reproduces
**3/3 identical colour hashes at p5743, p5958b and p6097**, and matches a fourth
run from an earlier batch. Before it, my own flags-off pair differed at 13/13
poses in two populations — max delta 1 at 6 poses (rounding) and 187–241 at 7
(the lightmap flip). So the colour rows above are real byte gates, not depth
proxies.

**Process note, and it cost me a re-measurement.** That commit's build swapped
`Runtime/DEMO` underneath a render batch of mine — the exact hazard S1d-2 §7
recorded. Every headline table in this section was therefore RE-RENDERED end to
end on the post-fix binary, and every number reproduced to the digit (void 13 /
5 / 5 / 413 100 / 14 163; fallback 716 141 px; both recess byte-equalities). The
flags-off gate was redone properly: a `git worktree` at the parent commit,
configured and built separately (its own `Runtime/DEMO`, never the shared one),
and run with cwd = the MAIN `Runtime/` so both binaries see the same uncommitted
`GREETS.FLD`.

## S1d-2d.10 WHAT I DID NOT DO

- **A converged reference of closed-shell semantics** (gate 4). See above.
- **A clean perf bench.** The machine was loaded the whole session.
- **Cross-material welding.** `--pom_shell_weld=2` pins instead, and pinning
  destroys the floor's shell on greets. Welding `rooms` and `floor` offsets
  together (a 45° bisector at the wall/floor line) is the untried option, and it
  is what the last 14 163 px of void are waiting on.
- **`--pom_shell_side_edge` under the lid.** The per-side TRUE-BOUNDARY table is
  WRONG there: `PomShell_Build` runs once per material, so `floor`'s seam census
  sees `rooms` ALREADY DISPLACED and mis-classifies 19 of its 24 sides as free
  edges (`cop=5 in=19 out=0 true=0` under recess vs `cop=4 in=1 out=0 true=19`
  under the lid). Harmless for side FACES (floor's leans are 0 either way) and
  for `rooms` (it is built first), but `--pom_shell_side_edge=1` must not be used
  with the lid until the topology snapshot is taken once for the whole scene.

---

# S1d-2e — THE SWIM, MEASURED (and it is the GRAZING CAP), plus the cross-material weld

Added 2026-08-05, branch `fog-wt`. Two user-reported defects from a live review
of the per-pixel arms: *"the lid protrusion looks fantastic — when it actually
works. we have wall gaps, swimming textures, etc. for the other one, we still
have swimming textures."* Swimming is present in BOTH arms, so it is in the
march/shading, not the shell geometry. It is a MOTION artefact; nothing below
was measured on a still.

## S1d-2e.0 INSTRUMENT — a camera sweep, and a surface coordinate with no camera matrices

**The sweep.** 16 consecutive frames along a pinned free-cam path, one `./DEMO`
run per frame, dummy drivers, 1080p:

| sweep | path | per-frame camera delta | t |
|---|---|---|---|
| **A** | p5958a → p5958b (pure lateral dolly, near-parallel to the wall) | 0.040 world | 5958 |
| **B** | p5743 → half way to p5773 (dolly + yaw) | 0.052 world + 0.21° | 5743 |

Both come straight out of `docs/greets_review_poses.txt`; sweep A is the pose
the grazing smear was reported at, and its per-frame delta matches the demo
camera's own speed there (1.57 world over 30 t between p5743 and p5773).

**The surface coordinate.** The same arm plus **`--parallax_max_offset=0.000001`**
clamps the final parallax offset to a millionth of a texel, so its
`FDS_DUMP_TXTR` dump is the pixel's **GEOMETRIC UV**. That is a per-pixel
surface coordinate on the authored plane, identical in every recess-family arm
(none of them moves a vertex), so the SAME SURFACE POINT can be compared across
frames with no camera matrices, no depth unprojection and no search. Everything
below scatters a per-pixel quantity into a UV grid keyed on it.

**The metric.** Correct POM returns the first intersection of the **TRUE view
ray** with the height field. `--pom_ref_march --pom_ref_steps=512
--pom_shell_cap=64` is exactly that: 512 uniform steps + a secant solve, and cap
64 IS the kernel's own `1/max(V·N, 1/64)` ceiling, i.e. **no cap at all**. So

```
E(p)  = marchedUV_arm(p) − marchedUV_trueray(p)        [texels, same frame+pixel]
SWIM  = |E(f−1) − 2·E(f) + E(f+1)|   registered on the surface
```

A static E is a static distortion and does not swim; the second temporal
difference annihilates any smooth (linear-in-time) component — view-dependent
ramps, the legitimate parallax motion — and leaves the jumps.

## S1d-2e.1 THE ORDERED HYPOTHESES, cheapest first. Four of the five are dead.

**1. `--pom_height_mip` (per-FACE height mip) — DEAD, structurally.**
`--mips` **defaults to 0**, and `MiplevelClipper` ends every branch with
`if (!FeatureFlags::mips()) g_MipLevel = 0`. Measured from the G-buffer's own
mip nibble (`mat32 >> 28`): **every deferred pixel of every one of the 13 review
poses is mip 0** — not just the stone, the whole frame. Over sweep A, **0 of
29 484 424 stone pixel-pairs change mip.** `--pom_height_mip=0` is byte-identical
to the default; `=1` is slightly worse (swim jerk p90 0.83 → 1.40 texels). The
flag cannot be the cause at any pose the user reviews from, and the diagonal
seam it was written for cannot occur while `--mips` is off.

**2. March under-convergence — DEAD.** Cone-32 against the 512-step uniform +
secant reference at the SAME cap, sweep A, stone pixels:
**p50 0.02, p90 0.08, p99 0.5 texels.** `--parallax_pom=128` reproduces cone-32
to the digit. The shipping march is converged.

**3. `--parallax_pom_quarter` / 4. `--parallax_pom_lod`** — both default 0 and
neither is in the user's recipe. Not active.

**5. Clamp/fallback state flips — real, and secondary.** Pixels whose marched UV
equals the geometric UV exactly (the `--pom_recess_edge=0` clamp, plus any cone
march that never bracketed) are **3.4–4.8 % of stone pixels** on sweep A and
5.6 % on sweep B, and **1.2–2.6 % of stone pixels change that state per frame**.
It is a contiguous vertical band at the convex ridge, and it sweeps across the
wall as the camera moves (`docs/img/s1d_swim/A_flatclamp_mask_f00_f08_f15.png`).

**6. THE GRAZING CAP (`--pom_shell_cap`) — this is the dominant term, and it was
not on the list.** `rayScale = uvAmp × min(1/(V·N), cap)` leaves the ray's UV
AZIMUTH correct and clamps its RATE, so a capped ray dives into the height field
too steeply and lands on a nearer feature. Sweep A, error against the true ray:

| cap | \|E\| mean | \|E\| > 4 tx | **SWIM (jerk of E), mean** | jerk > 8 tx | flat-clamp area |
|---|---|---|---|---|---|
| 8 | 31.9 tx | 32.8 % | 12.64 tx | 7.71 % | 1.96 % |
| **16 (the user's arm)** | **37.5 tx** | **21.4 %** | **12.64 tx** | **7.07 %** | **3.96 %** |
| 32 | 22.1 tx | 6.8 % | 10.86 tx | 3.82 % | 8.92 % |
| **64 (= uncapped)** | **0.08 tx** | **0.13 %** | **0.34 tx** | **0.37 %** | **12.81 %** |

At the user's cap, **21 % of the wall is showing relief from a ray that is up to
228 texels (p99; 426 max) away from the true one, and that error moves 6.5
texels every frame.** The capped population is not static either: it grows
8.7 % → 14.5 % of stone across the 16 frames of the sweep. Raising the cap to 64
takes the swim to **1/37th** of the user's arm and the error to sub-texel.

Two controls that make the attribution airtight: the 512-step reference at cap
16 swims **exactly as much as the shipping cone march** (12.37 vs 12.64), and at
cap 64 both go to zero. So it is the cap, not the march, not the budget, not the
map resolution.

**It is a GRAZING phenomenon.** On sweep B (an ordinary 3/4 view) the cap binds
on only 2.23 % of surface points, swim is 1.27 tx, and raising the cap to 64
costs 0.06 pp of clamp area. The wall poses the user reviews from are mostly
near-parallel, which is why he sees it.

## S1d-2e.2 LOOK — before any number

`docs/img/s1d_swim/`, all from sweep A (t=5958), the crop is the grazing band on
the right wall (screen 700,120–1120,960).

- **`A_swim_cap16_vs_cap64.gif`** — the deliverable. 16 consecutive frames,
  **left = `--pom_shell_cap=16` (the user's arm), right = `--pom_shell_cap=64`**,
  110 ms/frame, looping. Open it in any browser or Preview. This is a motion
  artefact; the still below only hints at it.
- `A_swim_filmstrip_cap16_top_cap64_bottom.png` — 6 of those frames, cap 16 on
  top, cap 64 underneath, if a GIF is inconvenient.
- **`A_band_cap16_cap64_true512.png`** — the same frame three ways at full res:
  `cap 16` | `cap 64, cone-32` | `cap 64, ref-512`. The left panel's stone is
  **smeared into vertical ribbons**; the middle recovers the mottling and the
  block structure; the middle and right are indistinguishable, which is the
  picture of "cone-32 at cap 64 IS the converged answer".
- `A_caperr_map_f08.png` — where the cap binds, over the frame. A broad vertical
  band down the receding part of the wall, orange = the landed UV differs from
  the true ray's by more than 4 texels.
- `A_flatclamp_mask_f00_f08_f15.png` — the clamp band (magenta) at frames 0/8/15.

## S1d-2e.3 WHAT TO DO — no code change, one flag value, and the honest trade

`--pom_shell_cap=64` on the standing recess arm:

| gate | cap 16 | cap 32 | cap 64 |
|---|---|---|---|
| void, 13 review poses | **5** | **5** | **5** |
| swim (jerk of E vs the true ray), sweep A | 12.64 tx | 10.86 | **0.34** |
| clamp/fallback area, sweep A | 3.96 % | 8.92 % | **12.81 %** |
| clamp/fallback area, sweep B | 5.57 % | — | **5.63 %** |
| offscreen deltas | zero by construction (recess-only moves no vertex) | | |
| ms/frame, greets t=5958, min of 25, 3 interleaved rounds | 51.53 / 51.22 / 51.08 | 52.23 / 51.34 / 51.48 | 51.74 / 50.78 / 51.00 |

Perf: the within-arm spread (0.45 ms) exceeds the between-arm difference at load
average 7–14, so the cap's cost is **below my measurement resolution, ≤ ~0.5
ms/frame** — not zero, and I am not calling it zero. It is cheap for a
mechanical reason: `--pom_march_earlyout` breaks the loop when every lane has
bracketed, and the cone step is proportional to the gap, so a longer ray takes
BIGGER steps, not more of them.

The trade is the one this whole document is about, seen from the swim side: a
longer ray is a more correct ray AND a ray more likely to leave its patch. Cap
16 buys a smaller clamp band by rendering the wrong surface on 21 % of the wall;
cap 64 renders the right surface and clamps three times as much of it flat at
grazing. **Neither is correct, and the closed shell is still what removes the
choice.** What is new is that the cost of the cap is now a measured number
instead of a quality knob, and it is the thing the user is seeing move.

Not proposed as a default: `--pom_shell_cap` defaults to 8 and the user flips
defaults.

## S1d-2e.4 THE OTHER THING IN THE FRAME, stated because it is not the march

`--texture_filter` defaults to **0**, so the G-buffer carries a 20-bit INTEGER
swizzled texel address and the deferred kernel point-samples albedo, AO,
roughness and the normal map at texel granularity. At every greets wall pose the
stone is MAGNIFIED — measured on sweep A, one height texel covers ~15 screen
pixels — so the wall is drawn as ~4 px texel blocks that crawl under camera
motion (`Z_zoom` comparison: `--texture_filter=1` is visibly smooth where the
default is blocky). The flag's own help calls this "full-facade texture crawl".
It is present in flat POM and in `--no-parallax` alike, so it is NOT specific to
the displacement arms, and on the surface-registered appearance metric
`--texture_filter=1` moved the recess arm's colour jerk by under 10 %. I record
it because it is a real, visible, always-on temporal artefact on these surfaces
and the user may well be seeing it mixed in with the cap's — but I did not
demonstrate that it is what he means, and I am not going to claim it.

## S1d-2e.5 CROSS-MATERIAL WELD (`--pom_shell_weld` 3/4/5/6) — built, measured, DOES NOT meet the gate

The task's premise: the lid arm's residual 14 163 void px are geometric, at
CROSS-MATERIAL junctions, because `PomShell_Build` runs once per material and
`--pom_shell_weld=1` can only weld within one. `PomShell_WeldPrepare` now takes
ONE scene-wide position bucket over every material about to be shelled, before
the first build moves a vertex, and memoises the offset VECTOR applied at each
position so two materials with different amplitudes still land a shared corner
on exactly one point. Modes: **3** = scene-wide weld, **4** = 3 + the true mitre
(`off / cos(half-fold)`, so each incident plane is offset by exactly `off` and
nothing retracts), **5** = 3 + pin against UNSHELLED neighbours only, **6** = 5 +
the mitre. All default OFF.

| arm | void, 13 review poses |
|---|---|
| flat POM / recess / lid `--pom_shell_lid_probe` | 5 / 5 / **10** |
| **lid, `--pom_shell_weld=1` (the standing arm)** | **14 163** |
| lid `--pom_shell_weld=3` | 24 334 — **worse** |
| lid `--pom_shell_weld=4` | 51 012 — much worse |
| **lid `--pom_shell_weld=5`** | **10 646 (−25 %)** |
| lid `--pom_shell_weld=6` | 26 765 — worse |

**The premise is wrong, and the measurement says why.** The weld does what it
was built to do — all 42 rooms/floor positions weld, the floor keeps its shell
(0 pinned, 90 of 90 verts reuse the wall's delta, against `--pom_shell_weld=2`'s
90 of 90 PINNED and no shell at all) — and the void goes UP. Welding a corner to
the mean normal RETRACTS the surface laterally by `off·(1−cos(half-fold))`,
which pulls the floor 0.064 world in from its **UNSHELLED** neighbours (the
mirror panel, `siling`, the columns, the stairs) and opens more slit than the
rooms/floor junction it closes. Mode 5 pins exactly those and is the only
variant that helps. **The dominant residue is a shelled-to-UNSHELLED junction,
where there is nothing on the other side to weld to** — so the way out is
shelling the neighbours, or mode 5's pin, not a better weld.

**A control correction that matters for §S1d-2d.** That section attributes
13 986 of the 14 163 to geometry because they "survive `--no-parallax`".
`--no-parallax` also drops the stone HEIGHT MAPS, so `PomShell_Build` is never
called and there is no shell at all — measured, that arm voids 13 992 px on its
own, and it cannot attribute anything. The valid control is
`--pom_shell_lid_probe` (lid offset forced to 0, every other path bit-identical):
**10 px**. It does confirm the void is the moved vertices, by a different route.

### Gates

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (`4ac809e5` / `b41894f9` / `166fa25a`) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets **lid** arm t=5743 depth, new flags OFF | `b2a1aa7f...` — byte-identical to a `6987ffa` worktree build |
| greets **recess** arm t=5743 depth, new flags OFF | `26507444...` — byte-identical, same method |
| bad flags | **0** across every run log |

**A byte-nullity trap worth the space.** The first two attempts were NOT
byte-null with the flag off: naming a temporary for the vertex delta breaks the
`-ffp-contract=fast` multiply-add the compiler folds `Pos += vn/nl*off` into,
and every vertex moved by an LSB — lid depth `b2a1aa7f` → `c748436b` at t=5743
with `--pom_shell_weld=1`. The armed path is now a wholly separate branch and
the original statements are textually untouched. If a "no-op" refactor of a
vertex move ever fails a depth gate, look here first.

**And a process one.** A concurrent session committing on this branch swept my
uncommitted `DEMO/DisplaceRebuild.cpp` hunk into its own commit (`18a58ae`)
without the declarations, leaving HEAD unable to compile for the ~30 minutes
until I committed the rest.

---

# S1d-2f — THE MITRE INVERSION, ROOT-CAUSED (and it is TANGENTIAL SLIDE)

Added 2026-08-06, branch `fog-wt`. `S1d-2e.5` recorded that the geometrically
CORRECT true mitre (`--pom_shell_weld=4`, 51 012 void px) measures much worse
than the mean-normal approximation it was meant to replace (`=3`, 24 334) and
than the per-material weld (`=1`, 14 163), and called the disagreement
"unexplained … a real open bug". `DISPLACEMENT_RESEARCH_II` §8.5 R2 made
resolving it a precondition of the prism. This section resolves it.

**The answer: the mitre is exact on the component nothing needed and strictly
worse on the component that opens the holes.** It is not a bug in the
implementation. The construction is doing precisely what a mitre does, and a
mitre is the wrong objective for this arm.

## S1d-2f.0 THE MECHANISM, stated before the numbers

At a fold of half-angle `T` between two incident planes, welding moves the
shared corner along the bisector `m`. Decompose that move against ONE incident
plane, normal `N`, `m·N = cos T`:

| construction | move | NORMAL part (`d·N`) | TANGENTIAL part (`|d − (d·N)N|`) |
|---|---|---|---|
| mean normal (`weld=1/3/5`) | `off·m` | `off·cos T` | **`off·sin T`** |
| true mitre (`weld=4/6`) | `off·m / cos T` | `off` | **`off·tan T`** |

The mitre's guarantee is the middle column: every incident plane ends up offset
by exactly `off`. The cost is the right column, and `tan T > sin T` for every
`T`, so **the mitre is a strict increase in tangential slide, by exactly
`1/cos T`** — up to the implementation's 3× clamp, which on greets' measured
78.7° folds means a corner travelling `3 × 0.09 = 0.27` world.

The normal part is worth nothing here. `Vertex::ShellH` already records the
height each corner actually reached (`0.5 + 0.5·ndv`), so the march enters at
the true geometric height whether the corner reached `off` or `off·cos T` —
that is what ShellH is for, and `--pom_shell_weld`'s own flag help says so. The
tangential part is pure damage: it slides a patch's BOUNDARY sideways inside
its own plane, dragging every free edge away from whatever was meeting it.

**The formula in `S1d-2e.5` and in RESEARCH_II §8.5 R2 —
"retracts by `off·(1−cos(half-fold))`" — is wrong and is retracted.** The
retraction is `off·sin T`. At the 45° folds that dominate greets that is
`0.09 × 0.707 = 0.0636`, which is the `0.064 world` that section MEASURED;
its own formula gives `0.0264`, 2.4× too small. The number was right, the
formula was not.

## S1d-2f.1 THE VOID IS GEOMETRY, NOT THE MARCH — measured first, because it
## rules out every march-side hypothesis at once

`--pom_path_viz=2` writes a per-pixel path code on the PRE-KILL coverage mask,
so a fragment the march discarded is still recorded and code `0` means *no
fragment was ever rasterised at this pixel*. Void (`z16 == 0`) split on that,
16 review poses, standing lid arm, RelWithDebInfo build of `0846811` + this
section's census flag:

| extra void of … over `weld=1` | px | **NO FRAGMENT (geometry)** | fragment then killed |
|---|---|---|---|
| `weld=0` (no weld) | 220 897 | **216 233 (97.9 %)** | 4 664 |
| `weld=3` | 13 009 | **12 950 (99.5 %)** | 59 (all `keep`, i.e. a farther face won) |
| `weld=4` | 42 658 | **42 141 (98.8 %)** | 517 |
| `weld=5` | 10 632 | **10 632 (100 %)** | 0 |

Not one of the extra pixels is a domain exit, a base clip or a no-cross
discard. **The weld modes differ in geometry only.** Every march-side
explanation (ShellH, the entry height, the cap, the domain box, the side
faces) is excluded by this table without needing a separate experiment.

## S1d-2f.2 WHERE, AND ON WHICH FACES

Void by pose, all 16 review poses, every mode re-measured on the current tree:

| pose | `weld=0` | **`=1`** | `=3` | `=4` | **`=5`** | `=6` |
|---|---|---|---|---|---|---|
| p1 t5743 | 534 | 153 | 428 | 617 | 284 | 436 |
| p2 t5773 | 189 | 159 | 368 | 364 | 241 | 206 |
| p4 t5843 | 18 360 | 1 | 0 | 5 | 0 | 5 |
| p5 t5963 | 96 747 | 8 700 | 10 812 | 14 363 | 4 585 | 170 |
| **p6 t6133** (mirror) | 13 359 | 3 509 | 8 548 | **24 181** | 3 865 | 17 864 |
| **p7 t6293** (mirror) | 4 592 | 1 641 | 4 178 | **11 482** | 1 671 | 8 084 |
| p9 t5958 | 86 131 | 0 | 0 | 0 | 0 | 0 |
| p11 t5958 | 8 489 | 0 | 0 | 0 | 0 | 0 |
| p14 t5534 | 5 | 0 | 7 | 10 | 2 | 9 |
| p16 t5854 | 4 196 | 0 | 0 | 0 | 0 | 0 |
| others (6 poses) | 10 | 0 | 0 | 0 | 0 | 0 |
| **TOTAL** | **232 612** | **14 163** | **24 341** | **51 022** | **10 648** | **26 774** |

The 13-pose subset reproduces S1d-2e.5 to the digit (14 163 / 24 334 / 51 012 /
10 646), so this is the same phenomenon, re-measured.

**70 % of `weld=4`'s void is at the two MIRROR-PANEL poses**, and it is ONE
crack. `--face_id_dump` at p6 t6133, resolving the faces immediately above and
below every extra-void pixel:

```
material ABOVE the crack: rooms::mirUV 13 108 | floor::mirUV 6 135 | rooms 413
material BELOW the crack: floor::mirUV 18 000 | rooms 82 | rooms::mirUV 76
```

a single line across the floor, 17 875 of the 21 881 extra pixels inside
`y ∈ [820, 900]`. `weld=3` opens the SAME line, 5 093 px — narrower, in the
same place, at the same faces.

**LOOK: `docs/img/s1d_2f/mitre_crack_p6_t6133.png`** — the same 600×180 crop of
that floor line at `weld=1 | =3 | =4`. Mode 1: no crack. Mode 3: a thin black
line the width of the room. Mode 4: the same line, several times thicker. That
is the whole finding in one picture, and the widths are `off·sin T` and
`off·tan T`.

The three faces at the crack, pristine vs each mode (from `--face_id_dump`,
`--pom_shell_lid_probe` for pristine):

```
Piramid.lwo:c0  floor::mirUV
  pristine  A=(17.898,0.000,-49.374)  B=(17.898,0.000,-75.913)  C=(49.374,0.000,-75.913)
  weld=1    A=(17.898,0.090,-49.374)  B=(17.898,0.090,-75.913)  C=(49.374,0.090,-75.913)   |d| 0.0900 / 0.0900 / 0.0900
  weld=3    A=(17.958,0.060,-49.404)  B=(17.935,0.037,-75.839)  C=(49.337,0.073,-75.876)   |d| 0.0900 / 0.0906 / 0.0898
  weld=4    A=(17.988,0.090,-49.419)  B=(17.988,0.090,-75.733)  C=(49.284,0.180,-75.823)   |d| 0.1350 / 0.2205 / 0.2205
```

`weld=1` is a **RIGID TRANSLATION** of the floor — every vertex `(0, +0.09, 0)`,
the plane offset by exactly the slab half-depth and not moved one micron
sideways. `weld=3` gives each position its own scene-wide mean direction, so the
same three corners rise by 0.060 / 0.037 / 0.073 and slide 0.03–0.07 sideways.
`weld=4` divides each of those by its own `cos T` and the far corner rises
0.180 — twice the offset — while `B` slides **0.180 world in z**.

Both faces above the crack meet `c0` at a **T-JUNCTION**: `c19`'s vertex
`(17.898, 0, -62.952)` and `c50`'s `(17.898, 0, -58.014)` both lie in the
INTERIOR of `c0`'s edge `A–B` (`x = 17.898`, `z ∈ [-49.374, -75.913]`), not at
its corners. A T-junction is watertight under any offset that is AFFINE ALONG
THE EDGE — a rigid translation is — and opens by the deviation of the middle
vertex from the moved line as soon as it is not. Evaluated by hand at
`c19`'s B: **`weld=1` gap 0.000, `weld=3` 0.041, `weld=4` 0.061 world.**

## S1d-2f.3 THE PREDICTOR: `--pom_shell_slit_census`

New diagnostic flag, default OFF, init-time print only, and deliberately
implemented WHOLLY OUTSIDE `PomShell_Build` so that function's
`-ffp-contract=fast` vertex move is textually untouched (the trap S1d-2e.5
documents). It snapshots every vertex position before the first build and after
the last one attributes, per COPY, the delta actually applied.

The metric that ranks the modes is **(D) TANGENTIAL SLIDE against the vertex's
own PRISTINE face plane** — `|d − (d·N)N|`, which is exactly 0 for a rigid
offset and needs no neighbour and no shared vertex, so unlike a position-keyed
census it also sees a boundary that merely ABUTS another surface:

| mode | `floor` slide | `rooms` slide | all, mean | all, max | shelled-vs-shelled TEAR | **void, 16 poses** |
|---|---|---|---|---|---|---|
| 0 (no weld) | 0.0000 | 0.0030 | 0.0026 | 0.0374 | **13.02** | 232 612 |
| **1** | **0.0000** | 0.0364 | 0.0316 | 0.0883 | 5.21 | **14 163** |
| 3 | 0.0594 | 0.0418 | 0.0441 | 0.0885 | 0.00 | 24 341 |
| 4 | 0.1050 | 0.0656 | 0.0708 | **0.2648** | 0.00 | 51 022 |
| **5** | 0.0598 | 0.0419 | 0.0450 | 0.0885 | 0.00 | **10 648** |
| 6 | 0.1055 | 0.0641 | 0.0712 | 0.2635 | 0.00 | 26 774 |

Read it as `void ≈ TEAR + SLIDE + unshelled-junction gap`:

- **mode 0 is the TEAR** (13.02 world of pairwise disagreement between the
  copies at one position) and almost no slide — each vertex moves along its own
  normal, which is tangential to nothing. That is the 232 612.
- **the weld converts tear into slide.** Once the tear is gone the void tracks
  the slide, and it tracks it *within each material*: `floor` 0.0000 / 0.0594 /
  0.1050 and `rooms` 0.0364 / 0.0418 / 0.0656 for modes 1 / 3 / 5→ order
  preserved in both.
- **`weld=1`'s floor slide is EXACTLY ZERO** because a per-material weld on a
  planar material is a rigid translation. That single property is why mode 1
  beats mode 3 despite mode 3 closing the *entire* cross-material tear (5.21 →
  0.00). Scene-wide welding is the operation that destroys it: the build log
  goes from `'floor': … 0 corner verts (ShellH min 1.000)` to
  `90 corner verts (ShellH min 0.621)` — every one of the floor's 90 vertex
  uses becomes a welded corner, because all 42 of its positions are shared
  with `rooms`.
- **the mitre is a pure multiplier on the slide.** The cleanest pair is 5 vs 6:
  identical pin set, identical topology, zero tear and zero unshelled gap in
  both, the only difference being the `1/cos T` factor. Slide 0.0450 → 0.0712
  (**×1.58**), T-junction gap area 79.9 → 161.0 (**×2.02**), void 10 648 →
  26 774 (**×2.51**). The 3/4 pair says the same: slide ×1.61, |d| mean
  0.0900 → 0.1323 (×1.47), void ×2.10.
- **mode 5 wins because it removes a different term** — the gap against
  UNSHELLED neighbours, `siling` and `teleporter`, which the census measures at
  slide-area 95.2 world² for modes 1/3 and 0.0 for modes 5/6.

## S1d-2f.4 VERDICT, and what it changes downstream

**`--pom_shell_weld=4/6` should not be pursued and the "geometrically correct
construction measures worse" open bug is closed.** The mitre optimises
per-plane offset EXACTNESS. Nothing in this pipeline consumes that — ShellH
already carries the height each corner reached — and it buys it by multiplying
the one quantity that opens holes by `1/cos(half-fold)`. There is no tuning of
the 3× clamp that rescues it: at the clamp the slide is 3× worse, and below the
clamp it is still `tan/sin = 1/cos` worse than mode 3. The correct objective for
a lid offset is **minimum tangential slide**, and on a planar patch the minimiser
is the rigid translation `weld=1` already performs.

Three consequences for `RESEARCH_II` §8.6:

1. **Precondition 1 must not say "mitre".** It should say: one extrusion
   DIRECTION per position (Hirche req. 1, still required so two prisms share
   their side quad), chosen to minimise tangential slide — not one that puts
   every incident plane at exactly `off`.
2. **The mitre's problem largely disappears under a real prism, and that is an
   argument FOR the prism.** The whole weld/mitre difficulty exists because the
   lid-only shell has nothing to seal a fold with, so the two lids are required
   to meet. A prism has a shared SIDE QUAD at every interior edge: two adjacent
   prisms stay watertight while their lids move apart, because the side quad
   follows the vertices. What survives is only the boundary against geometry
   that is NOT a prism — §8.6 precondition 3 — plus T-junctions, which need
   the edge split, not the weld, to be consistent.
3. **T-junctions are a new precondition.** greets' floor carries at least two
   (`c19`, `c50` on `c0`'s 26.5-world edge) and the census finds 140
   (edge, T-vertex) pairs among the shelled faces alone. Hirche's manifold
   requirement (3) already excludes them in principle; this is the first
   measurement of how many greets actually has, and they are load-bearing —
   they are where 70 % of `weld=4`'s void lives.

## S1d-2f.5 THE WELD ITSELF — TAKEN TO DEFAULT ON

Re-measured over the current 16 review poses, not the 13 the −96.6 % figure
came from: **232 612 → 14 163 px, −93.9 %**, for a pure geometry change with
the march untouched. It removes void ENTIRELY at five poses where the unwelded
lid loses 4 196 – 96 747 px, and `=5` takes it to **10 648, −95.4 %**.

LOOK, not the pixel count — `docs/img/s1d_2f/weld_gash_p9_t5958.png` and
`weld_gash_p5_t5963.png`, crops of the void bbox, `weld=0 | =1 | =5` side by
side:

- **p9 t5958** — unwelded, a **full-height black gash** runs top to bottom of
  the frame between two wall panels. Welded, the wall is continuous and the two
  panels' stone reads across the join. This is the user's reported "black
  gashes between wall panels", and the weld is what removes it.
- **p5 t5963** — unwelded, a large black wedge takes the bottom-left of the
  crop where the wall meets the floor, plus a torn strip up the wall's lower
  edge. Welded, both are gone.
- `=1` and `=5` are visually indistinguishable at these two poses; `=5`'s win is
  concentrated at p5 (8 700 → 4 585).

**`--pom_shell_weld` default flipped 0 → 1.** The campaign's standing rule is
that the user flips defaults (S1d-2e.3 explicitly declined to), and the reason
this one is taken rather than proposed is that **it cannot change anything he
has not already opted into**: the weld is inert unless `--pom_shell` selects
the lid, and `--pom_shell` is itself default OFF, so no shipping render and no
pinned scene can move. It is also inert under `--pom_recess_only` and
`--pom_shell_lid_probe` by construction (`weldLid` tests both). Within a lid
arm the change is not a quality knob: the unwelded lid is a **torn mesh**, the
tear is 13.02 world of pairwise disagreement between copies of the same corner,
and the pixels it costs are the exact defect the user reported. `=0` still
restores the torn lid for A/B.

**`=5` is NOT taken**, and the reason is stated rather than hidden: it is 25 %
better on void (10 648 vs 14 163) but it does so by PINNING 165 of `rooms`'
588 vertex uses and 3 of `floor`'s 90 — those corners get no lid at all
(`ShellH` 0.5, no relief above the authored plane). I compared `=1` and `=5`
only at the two gash poses, where they are indistinguishable; **I did not
examine the 168 pinned corners**, so I have no basis to claim the pin is free.
It stays an explicit opt-in for the standing arm until someone looks at them.

## S1d-2f.6 GATES

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (`4ac809e5` / `b41894f9` / `166fa25a`) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact, 4/4 |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact, 3/3 |
| greets pin | `f1297141611c484bac7cc10a8bdcf630` — byte-exact, 4/4 |
| `--pom_shell_slit_census` default | 0; `PomShell_SlitSnapshot` returns before touching anything, `PomShell_SlitCensus` before reading anything |
| `PomShell_Build` | not edited by this section — the census lives in two new functions and the FP-contract trap of S1d-2e.5 cannot recur |
| bad flags | 0 across all 100+ run logs (`unknown flag` / `requires a value` grep) |
| **after `--pom_shell_weld` default 0 → 1** | all four gates re-run and still byte-exact: render_gate 3/3, city `37e62845` 3/3, fountain `51fff7cd` 3/3, greets `f1297141` 3/3 — the weld is inert without `--pom_shell`, which none of them uses |
| new default == explicit `=1` | greets lid arm at p6 t6133, no weld flag vs `--pom_shell_weld=1`: colour `589c2003` and depth `f4a88f9b` identical |

## S1d-2f.7 REPRODUCTION

```sh
LID="--deferred --pom_shell --pom_shell_side_faces=3 --pom_shell_lid_edge=1 \
 --no-pom_shell_base_clip --pom_shell_world_amp --pom_shell_world_amp_set=0.18 \
 --pom_normal --parallax_pom_cone --parallax_pom=32 --pom_cone_exact=1 \
 --pom_cone_min_step=1 --pom_march_earlyout --pom_shell_cap=16"

# the census — no rendering needed, init-time print
./DEMO --snapshot=greets@t=5743 $LID --pom_shell_weld=N --pom_shell_slit_census

# void + cause split, per pose from docs/greets_review_poses.txt
FDS_SNAPSHOT_ZDUMP=1 FDS_GREETS_CAM="<cam>" \
  ./DEMO --snapshot=greets@t=<t> --out=<dir> $LID --pom_shell_weld=N --pom_path_viz=2
#   void  = popcount(z16 == 0)
#   cause = path.u32 == 0  ->  no fragment (geometry);  != 0 -> march, [7:4] = action

# the faces at the crack
FDS_GREETS_CAM="27.9341908,3.21640229,-59.6960106,-0.994124591,-0.0584560856,0.0910993442" \
  ./DEMO --snapshot=greets@t=6133 --out=<dir> $LID --pom_shell_weld=N --face_id_dump
#   pristine positions: the same run with --pom_shell_lid_probe
```

---

# S1d-3 — THE PRISM. Real side geometry, and it takes the lid arm's void to 2 px

Added 2026-08-06, branch `fog-wt`, flag `--pom_prism` (default 0 = OFF).
`S1d-2f.1` established the thing this stage acts on: **every void pixel the weld
cannot reach is `--pom_path_viz` code 0 — no fragment was ever rasterised
there.** It is missing geometry, and no march-side flag can answer it. Hirche
2004's answer is a prism per base triangle, whose side quads are rasterised
geometry rather than an exit test. This builds them.

## S1d-3.0 THE CONSTRUCTION, and the one rule it runs on

`PomShell_BuildPrism` (`DEMO/MeshOps.cpp`) emits a side quad at an edge iff

> no partner face shares its AUTHORED endpoints with the SAME offset delta at
> both of them.

That single test covers all four cases §8.6 lists separately: a free edge has no
partner; an unshelled neighbour has a zero delta; a T-junction has no partner
sharing the edge EXACTLY (the T splits it), so it is emitted too — precondition
5b falls out of the rule instead of needing its own pass; and a torn corner has
a partner whose delta differs. Under the (default) weld the rule reduces to the
BOUNDARY of the shelled surface, because interior edges already agree by
construction; with the weld off it emits the literal per-triangle prism.

The quad spans **lid (`Pos`) to base (`Pos − 2·delta`)**, so it passes exactly
through the AUTHORED edge. That is why it seals against a static neighbour
whatever the weld's tangential slide did — the slide moves both rings together
and the segment still contains the authored point. `ShellH` runs 1 → 0 down the
quad, so the march enters at the fragment's true slab height; `N`/`Tangent` are
the PATCH's, so the tangent frame, the UV domain and the material — including
its `::mirUV` handedness clone — are the neighbouring lid's. Built after the
handedness split and before the mirror clone build, into ONE new `TriMesh`
(`pom_prism_sides`, no "Piramid" in the name so the chunk split skips it).
Nothing existing is reallocated, so every `Face→Vertex` pointer stays valid.

greets, `--pom_prism=1`:

```
[POM-PRISM] mode=1: 226 shelled faces, 678 edges tested -> 196 side quads
            (784 verts / 392 faces). Cause: 87 FREE edge (no partner),
            43 partner does NOT move (unshelled / T-junction),
            66 partner moves DIFFERENTLY (torn). Skipped: 482 already watertight
```

**392 faces. The tessellation carve is 86 600** — the prism is 0.45 % of it.
`--pom_prism=2` (every edge, the literal Hirche set) is 678 quads / 1 356 faces.

## S1d-3.1 VOID — the headline, 16 review poses

`FDS_SNAPSHOT_ZDUMP=1`, `void = popcount(z16 == 0)`, dummy SDL drivers,
1920×1080, standing lid arm (`weld=1` default, `side_faces=3`, `lid_edge=1`,
`world_amp_set=0.18`, cone-32, `cap=16`).

| arm | void, 16 poses |
|---|--:|
| lid (the standing arm) | 14 163 |
| **lid + `--pom_prism=1`** | **2** |
| lid + `--pom_prism=2` (full per-triangle prism) | 3 |
| lid + `--pom_prism=1 --pom_shell_side_faces=0` | 2 |
| lid + `--pom_prism=1 --pom_shell_lid_edge=0` (pure discard) | 164 055 |
| **tessellation (`--greets_displace`)** | **156** |
| flat POM | 7 |

Per pose, lid → prism: p1 t5743 **153 → 0**, p2 t5773 159 → 2, p4 1 → 0,
p5 t5963 **8 700 → 0**, p6 t6133 (mirror) **3 509 → 0**, p7 t6293 (mirror)
**1 641 → 0**, every other pose 0 → 0.

Three things this table says, in order of importance:

1. **The prism beats tessellation on void by 78×** on the same 16 poses. The
   "0–24 px" the campaign has been quoting for tessellation is a 13-pose figure:
   re-derived on the current 16, tessellation is **156**, of which 142 sit at
   **p14 t=5534** — the corridor-looking-back pose the earlier sets did not
   cover. That is not a criticism of tessellation; it is the bar being measured
   on the full set for the first time.
2. **The full per-triangle prism buys nothing over the boundary skirt** (3 vs 2)
   for 3.5× the faces. Weld first, then seal only what the weld cannot: the
   ordering `S1d-2f.4` recommended is confirmed by measurement.
3. **`--pom_shell_side_faces` is now dead weight.** The analytic leaning side
   PLANES were S1d-2a's approximation of the side face. With the real quad
   present, turning them off changes the void by nothing (2 → 2). The prism
   replaces them.

And one thing it does NOT say: `--pom_shell_lid_edge=0` — the pure discard
§8.6 precondition 8 asks for — is **still catastrophic (164 055)**. The reason
is a scope mismatch, and it is worth stating because it is the honest limit of
this stage: the march's domain is each PATCH's UV BOX, while the prism seals the
shelled SURFACE's boundary. A ray that leaves its patch's UV box mid-wall still
has no neighbouring prism fragment to answer it. Closing that would mean a prism
per patch-box side, not per surface boundary. The clamp stays.

## S1d-3.2 IT DOES NOT TOUCH THE MARCH — proven, not asserted

At p9 t=5958 (the grazing pose), `FDS_DUMP_TXTR` marched-UV dump plus the depth
dump, lid vs lid+prism:

```
covered px 2 073 600
marched UV bit-identical on covered px: 2 073 600 / 2 073 600
depth identical px:                     2 073 600 / 2 073 600
```

**Every pixel of the frame, bit for bit.** So at this pose the prism adds
nothing and removes nothing, and — the point of the exercise — **SLIP under the
prism is exactly the lid arm's**, by construction rather than by a sweep. The
prism is not a swim fix and must not be sold as one.

Across the 16 poses the depth plane changes at 2 536 px (p1), 55 px (p9) and
**0 px (p14)** — it is depth-ADDITIVE. The final colour also moves by exactly
**±1 LSB over ~25 % of the frame at every pose** (p14: 397 513 px at ≤1, 26 px
above 16/255, max 39). The lid arm is byte-exact run-to-run (3/3 at three
poses), so that drift is real and caused by the extra mesh — a
lighting/accumulation-ORDER effect, not a march change, and it is reported here
rather than rounded away.

## S1d-3.3 THE CAP IS NOW A FREE PARAMETER — the one lever the prism unlocks

`--pom_shell_cap` bounds `1/(V·N)` in the true-ray march. It is simultaneously
the silhouette's reach and the grazing smear's cause (`S1d-2e`), and the campaign
has never been able to lower it because the silhouette was the only thing holding
the boundary together. Measured today:

| arm | cap 2 | cap 4 | cap 8 | cap 16 |
|---|--:|--:|--:|--:|
| lid | — | **14 163** | — | **14 163** |
| lid + `--pom_prism=1` | **2** | **2** | **2** | **2** |

**Without the prism the cap does not move the void at all** (14 163 at 4 and at
16 — the void is geometry, exactly as S1d-2f.1 said). **With the prism the void
is 2 at every cap from 2 to 16.** The cap is therefore decoupled from the void
and can be tuned purely against the smear.

LOOK at p9 t=5958, the smear repro, 800×600 crop, `flat | tess | prism cap16 |
prism cap4` — `docs/img/s1d_3/graze_p9_t5958.png`. The mortar joint reads: flat
a thin dark line, tessellation a crisp joint, prism cap16 a wide smeared brown
band, prism cap4 a narrower and darker band. **The prism at cap 4 is visibly
better than at cap 16 and still not tessellation.** The grazing step is not
fixed by this stage.

## S1d-3.4 GATES

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (`4ac809e5` / `b41894f9` / `166fa25a`) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact 3/3 |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact 3/3 |
| greets pin `t=1588` | `f1297141611c484bac7cc10a8bdcf630` — byte-exact 3/3 |
| `--pom_prism` default | 0; `PomShell_PrismSnapshot` returns before touching anything, `PomShell_BuildPrism` is behind `pom_shell() && pom_prism()>0` |
| `PomShell_Build` | not edited by this stage — the prism lives in two new functions, so S1d-2e.5's FP-contract trap cannot recur |
| bad flags | 0 `unknown flag` / `requires a value` across every run log |

### Gate 3 — OFFSCREEN. The prism inherits the lid's cost and adds ~nothing

Shadow cube, `--dump_shadowmap` from `build-dev` (`-DFDS_DEV=ON`), t=5743,
76 cube faces, 13 533 184 depth texels:

| pair | texels differing | >8/255 |
|---|--:|--:|
| `flat` vs `rec` (recess-only) | **0 (0.00 %)** | 0 |
| `flat` vs `tess` | 191 665 (**1.42 %**) | 191 550 |
| `flat` vs `lid` | 2 540 585 (18.77 %) | 36 509 |
| `flat` vs **`prism`** | 2 542 546 (**18.79 %**) | 37 647 |

The `lid` and `rec` rows reproduce `S1d-2d.5`'s published figures to the digit,
which is what makes the other two trustworthy. **The 392 side faces add 1 961
texels (+0.08 pp).** The 18.77 % is the LID's — moving vertices — and the prism
neither causes nor cures it. Recess-only's 0 remains the standard and
**tessellation at 1.42 % is 13× cleaner offscreen than any lid arm.**

### Gate — PERF

`--bench=scene@scene=greets,t=5780,iters=20`, 4 interleaved rounds, min of arm,
load 8–12 (shared box):

| arm | ms | Δ flat | Δ lid |
|---|--:|--:|--:|
| flat | 49.55 | — | |
| tessellation | 54.23 | +4.68 | |
| lid | 55.54 | +6.00 | — |
| **lid + prism** | **56.18** | +6.63 | **+0.63** |

**The prism's own cost is +0.63 ms** — 392 faces and their pixels. At this pose
**tessellation is 1.95 ms CHEAPER than the prism arm**; per `ENVDYN §A2` the
ordering flips at the corner/corridor poses, where the shell's per-pixel cost
falls and tessellation's per-face cost does not.

## S1d-3.5 LOOK — the three defects the user named

- **p5 t=5963** (`docs/img/s1d_3/gash_p5_t5963.png`, 220×340 crop, `lid | prism`)
  — the lid arm's full-height black gash between the wall panels is replaced by
  a real, shaded, textured strip: the side of the slab at the fold. This is the
  single biggest void item (8 700 px) and it is geometry answering geometry.
- **p6 t=6133 / p7 t=6293, the mirror panels**
  (`docs/img/s1d_3/mirror_p6_t6133.png`, `lid | prism | tess`) — the lid arm
  draws a black outline around the mirror rectangle (its left vertical edge and
  the reflected floor's horizontal edge). Under the prism both are gone. That
  ring was 5 150 of the lid arm's 14 163 px and it is NOT wall relief: it is the
  mirror wall's own moved footprint against its clone.
- **p9 t=5958, the grazing smear** — **unchanged.** See §S1d-3.3. The prism does
  not touch the march and the marched UV is bit-identical.

## S1d-3.6 VERDICT — does the prism beat tessellation?

**On the defect it was built for, decisively. On the whole comparison, it is a
split decision and the split is legible:**

| axis | tessellation | lid + prism | winner |
|---|---|---|---|
| void, 16 review poses | 156 | **2** | **prism, 78×** |
| the t=5743 gash / the panel gashes | absent | **absent** | tie |
| the mirror-panel ring (p6/p7) | absent | **absent** | tie |
| protrusion + see-through at the slab (p1 t5743) | 122 458 px | 58 999 px | tessellation, but they are different surfaces |
| **grazing step at t=5958** | **crisp** | smeared | **tessellation** |
| **offscreen shadow cube vs flat** | **1.42 %** | 18.79 % | **tessellation, 13×** |
| cost at t=5780 | **54.23 ms** | 56.18 ms | **tessellation, −1.95 ms** |
| cost at the corner/corridor poses (`ENVDYN §A2`) | cheaper than flat+7 | per-pixel, pose-dependent | pose-dependent |
| faces added | 86 600 | **392** | **prism, 220×** |
| slip / swim | none (geometry cannot swim) | the lid arm's, unchanged | **tessellation** |

**The prism is the right answer to the question it was asked.** The user's
complaint — "the lid protrusion looks fantastic when it actually works" — was
about the cases where it did not work, and those cases were holes. The holes are
gone: 14 163 → 2 px, for 392 faces and 0.63 ms, with the march untouched and
every shipping gate byte-exact.

**It does not, on its own, retire tessellation.** Tessellation still wins the
grazing step, the offscreen consumers by 13×, and the frame at the wall bench
pose. What the prism changes is the *shape of the remaining argument*: before
today the lid arm had a structural hole class that no amount of tuning could
close, and tessellation was the only arm without one. That is no longer true, and
the two remaining gaps are both attacks on the MARCH, not on the geometry —
and §S1d-3.3 shows the prism has already handed the campaign the lever
(a free `--pom_shell_cap`) that the first of them needs.

## S1d-3.7 REPRODUCTION

```sh
LID="--deferred --pom_shell --pom_shell_side_faces=3 --pom_shell_lid_edge=1 \
 --no-pom_shell_base_clip --pom_shell_world_amp --pom_shell_world_amp_set=0.18 \
 --pom_normal --parallax_pom_cone --parallax_pom=32 --pom_cone_exact=1 \
 --pom_cone_min_step=1 --pom_march_earlyout --pom_shell_cap=16"

# void, per pose from docs/greets_review_poses.txt
FDS_SNAPSHOT_ZDUMP=1 FDS_GREETS_CAM="<cam>" \
  ./DEMO --snapshot=greets@t=<t> --out=<dir> $LID --pom_prism=1
#   void = popcount(z16 == 0)

# the march-identity proof (p9 t=5958)
FDS_DUMP_TXTR=1 FDS_SNAPSHOT_ZDUMP=1 FDS_GREETS_CAM="12.263032,2.64180589,\
-55.9950409,0.0452918001,-0.0762010962,0.996063471" \
  ./DEMO --snapshot=greets@t=5958 --out=<dir> $LID [--pom_prism=1]
#   diff greets_t005958_uv.bin between the two runs

# offscreen (dev build only — does not overwrite Runtime/DEMO)
build-dev/DEMO/DEMO --snapshot=greets@t=5743 --dump_shadowmap $LID --pom_prism=1
#   /tmp/shadowmap_*.pgm, 76 cube faces

# perf
./DEMO $LID --pom_prism=1 --bench=scene@scene=greets,t=5780,iters=20
```

---

# S1d-4 — ATTACK THE MARCH: the prism-enabled cap sweep, and the two gaps re-scoped

Added 2026-08-07, branch `fog-wt`, HEAD `677733f`. No code change in this stage —
every number below is a flag setting or a measurement. Box load 3.6–6.9
throughout (my own renders); every render `SDL_VIDEODRIVER=dummy`, 1920×1080,
sequential, 0 `unknown flag` across every run log.

## S1d-4.0 THE INSTRUMENT, AND ITS CALIBRATION AGAINST THE PUBLISHED LADDER

SLIP = texels of texture sliding per frame at a FIXED SURFACE POINT. Per frame
the renderer dumps the marched UV (`_uv.bin`) and the camera-free geometric UV
(`_uvgeo.bin`); `offset = uv − uvgeo` is the parallax displacement AT a surface
point, keyed on the geometric UV (1 texel cells) so the SAME surface point is
compared across frames with no camera math. REACH = `|offset|`. Sweep A is
p9→p10 at t=5958, 16 frames, 0.040 world/frame lateral dolly, one `./DEMO` per
frame. Scripts in the session scratchpad (`sweepA.sh`, `slip.py`).

**Calibration, because a new script's numbers are worthless until it reproduces
a published one.** Recess arm, same flags as `S1d-2e`:

| quantity | published | mine |
|---|--:|--:|
| clean floor (flat POM) slip p99 | 0.60 | **0.57** |
| clean floor slip p90 | 0.01–0.12 | **0.094** |
| recess cap 2 reach p90 | 28.6 | **28.65** |
| recess cap 4 reach p90 | 53.8 | **52.98** |
| recess cap 16 reach p90 | 184 | **190.29** |
| recess cap 64 slip p90 | 15.3 | **15.234** |

Reach reproduces within 2 % and the clean floor lands on the published value.
The mid-cap slip **p99** runs ~2× the published figure, so **p99s below are on my
scale only** and must not be mixed with `S1d-2e`'s; p90 and reach are directly
comparable.

## S1d-4.1 THE PRISM-ENABLED CAP LADDER — the headline

Lid+prism arm (`S1d-3.7`'s `$LID` + `--pom_prism=1`), sweep A. At this pose the
prism is provably inert (`S1d-3.2`: marched UV bit-identical on all 2 073 600 px),
so this is the LID arm's ladder with the hole cost removed.

| arm | slip p50 | slip p90 | slip p99 | reach p90 (tx) | zero-offset | reach/slip (p90) |
|---|--:|--:|--:|--:|--:|--:|
| flat POM (floor) | 0.007 | **0.094** | 0.57 | 10.3 | 0.01 % | 109 |
| cap 2 | 0.044 | **0.254** | 0.98 | 28.7 | 0.51 % | **113** |
| cap 4 | 0.096 | **0.891** | 2.70 | 50.9 | 1.24 % | 57 |
| cap 8 | 0.136 | **2.530** | 7.99 | 94.4 | 2.44 % | 37 |
| **cap 16 (standing)** | 0.213 | **6.246** | 28.47 | 181.6 | 5.09 % | 29 |
| cap 32 | 0.218 | **12.386** | 75.93 | 327.5 | 12.65 % | 26 |
| cap 64 | 0.122 | **16.652** | 385.29 | 135.1 | 24.91 % | 8.1 |

**Slip grows ≈ cap^1.55 while reach grows ≈ cap^0.9, so reach-per-slip is
monotonically best at the LOWEST cap and there is no knee anywhere on the
ladder.** The prism freed the knob; the knob turns out to have no good position —
it only slides you along a smooth curve. That is the single most important
result of this stage and it is a negative one.

## S1d-4.2 WHAT THE CAP ACTUALLY BUYS — relief depth, banded at the slab

Marched-UV reach is texture parallax, not the thing the eye calls relief. This
measures DEPTH against the flat arm, MASKED to the stone (the flat arm's
`_uvgeo` validity) and BANDED, because `--pom_shell_cap` also bounds the shell
DEPTH WRITE at `amp*cap` and lumping that in would score an artefact as reach.
Unmasked/unbanded is the retired ">3 world units" background-detector trap
wearing a different threshold — my first cut made exactly that mistake and had
to be thrown away.

**t=5958 (the grazing pose), stone = 2 072 458 px:**

| arm | in slab | RECESS 0.18–0.5 behind | PROUD 0.18–0.5 front | **RUNAWAY >0.5** |
|---|--:|--:|--:|--:|
| tessellation | 97.2 % | 52 326 | 0 | **6 150 (0.30 %)** |
| cap 2 | 84.6 % | **0** | 292 751 | 26 370 (1.27 %) |
| cap 4 | 92.1 % | 31 096 | 107 320 | 25 884 (1.25 %) |
| cap 8 | 83.9 % | 296 280 | 1 771 | 34 662 (1.67 %) |
| **cap 16** | 73.1 % | 207 561 | 2 329 | **348 332 (16.81 %)** |
| cap 32 | 72.9 % | 164 419 | 25 943 | **370 251 (17.87 %)** |
| cap 64 | 72.9 % | 164 380 | 210 912 | 185 340 (8.94 %) |

Two findings, both new:

1. **`--pom_shell_cap=2` renders ZERO recessed pixels at the grazing pose.** It
   is a flat wall translated 0.09 world out. So the bottom of the slip ladder is
   not a usable setting — it buys its cleanliness by having no relief at all.
2. **At the standing cap 16, 16.81 % of the stone writes depth more than 0.5
   world from the flat wall, on a slab that is 0.18 world deep** (p1 = −2.363
   world, i.e. 13× the slab). This is the `|dz| ≤ amp*cap` bound doing exactly
   what the flag says it does, and it has not been reported before. Tessellation
   is 0.30 %. It is a MAIN-VIEW depth defect (shadow passes rasterise geometry,
   so it does not reach the 18.79 % cube figure) and it will be read by any
   depth-consuming post.

**t=5743**, for contrast: the ordering inverts — runaway falls 9.16 % (cap 2) →
0.91 % (cap 16), and the arm is PROUD-dominated (271 094 px at cap 16) where
tessellation is purely RECESSED (109 764 px, 0 proud). The two arms express
relief by opposite conventions, which is why comparing their recess counts alone
is unfair, and it is pose-dependent which cap is closest to tessellation.

## S1d-4.3 THE THREE MECHANISM CANDIDATES — all three refuted

**(a) The angle-dependent FADE re-evaluated WITH the prism — still loses.**
`--pom_shell_cap_fade` blends the true ray toward the offset-limited ray as
incidence approaches parallel. The earlier verdict was taken when clamped pixels
were holes; that objection is gone, so it was re-run. At MATCHED REACH:

| reach p90 | hard cap | slip p90 | fade | slip p90 | hard cap wins by |
|--:|---|--:|---|--:|--:|
| ~28 | cap 2 | **0.254** | cap16 fade 0.5 (27.1) | 1.000 | 3.9× |
| ~42 | ≈cap 3 | ~0.6 | cap16 fade 0.3 (41.7) | 2.195 | ~3.7× |
| ~59 | ≈cap 5 | ~1.3 | cap16 fade 0.2 (58.7) | 3.849 | ~3.0× |
| ~93 | cap 8 | **2.530** | cap16 fade 0.1 (93.3) | 10.377 | 4.1× |

**The fade loses to the hard cap by 3–4× on slip at matched reach even with the
prism on.** It does what it was built to do — zero-offset falls to 0.25–0.49 %
against the hard cap's 5.09 % — but that was only valuable while clamped pixels
were holes, and the prism already solved that. `cap32_fade0.3` and
`cap64_fade0.3` are indistinguishable from `cap16_fade0.3` (slip 5.90 / 5.54 /
5.95), i.e. the fade dominates and makes the cap irrelevant. Not recommended.

**(b) Height-field LOW-PASS — refuted, and it was my own leading hypothesis.**
The reasoning was that tessellation cannot swim partly because its relief only
exists at the subdivision lattice, i.e. it is inherently low-passed, and that
Tatarchuk's own authoring guide says to blur a height map that stretches. At
cap 16, pinning the height+cone mip:

| `--pom_height_mip` | slip p99 | reach p90 |
|---|--:|--:|
| 0 | 28.47 | 181.64 |
| 1 | 27.51 | 181.93 |
| 2 | 27.28 | 182.59 |
| 3 | 27.16 | 183.41 |

**An 8× linear blur of the height field moves slip by 4.6 % and reach by
nothing.** All four levels are distinct (the values move monotonically and
mip 2 ≠ mip 3), so the treatment applied. Slip is not caused by the height
field's high frequencies.

**(c) Discrete CLASS FLIPS in the clamp/fallback band — refuted.**
`--pom_path_viz=2` across sweep A, classes registered on the surface, steps
bucket excluded:

| arm | any flip | kind | action | cap bit |
|---|--:|--:|--:|--:|
| cap 16 | 1.779 % | **0.076 %** | **0.059 %** | 1.682 % |
| cap 4 | 0.935 % | **0.009 %** | **0.020 %** | 0.909 % |

**The march's terminal action changes class on 0.06 % of surface points per frame
at cap 16.** Essentially all of the "flips" are the `kPomBitCap` bookkeeping bit
toggling as `1/(V·N)` crosses the cap, which changes no appearance by itself.
There is no hidden discrete flicker.

**And the slip tail is NOT quantised.** If the excursion were the first-hit
hopping from one stone block to the next it would land on multiples of the
~256-texel block pitch. The tail of `|slip| > 8` at cap 16 decays smoothly —
294 229 in [0,16), 139 897 in [16,32), 29 826 in [32,48), 6 288, 4 996, 4 770 —
with no mode anywhere near 256. **Slip is a continuous excursion of a long ray,
exactly as `S1d-2e.1` concluded, and not a block-hop.**

## S1d-4.4 GAP 2 — the crisp step is a SEPARATE defect, and it is SHADING

`S1d-3.6` records tessellation rendering a crisp geometric step at t=5958 where
the march smears. Asked whether that is the same defect as the smear: **it is
not, and the cap is not its lever.**

First, the feature was mis-located. At the VERTICAL mortar joint (x≈730) the
march is not measurably worse than tessellation:

| arm | joint FWHM px | contrast | max slope | joint-crop \|dz/dx\| p99.9 |
|---|--:|--:|--:|--:|
| flat | 36 | 66.1 | 11.39 | 0.0051 |
| tessellation | 61 | 61.0 | 4.85 | 0.1062 |
| cap 4…64 | 47–65 | 49.3 | 4.13–5.38 | **0.1744** |

The march writes a LARGER depth step at the joint than tessellation. It is not
short of depth contrast.

The step he means is at the frame's top and bottom — the wall at its most
edge-on — found by differencing the two arms per 60×60 tile rather than by
guessing. There tessellation renders a **lit ledge with a dark shadowed strip
under it**; every march arm renders a faint smudge. In that region:

```
                  |dz/dx| p99.9   clamped-to-flat   |offset| p50 / p90
tessellation           0.0101           5.9 %            0 / 0
cap 4,8,16,32,64       0.0101           0.0 %         25.1 / 38.4   (identical)
```

**The march there is NOT clamped, marches 25 texels, and writes the SAME depth
gradient as tessellation — and caps 4 through 64 are identical, so the cap does
not bind at all.** What tessellation has and the per-pixel arm does not is
shading off real relief: true geometric normals plus relief SELF-SHADOWING (the
lit top and the dark side). `--pom_horizon` is the per-pixel arm's only
self-shadow term and it is OFF in the standing recipe. **Gap 2 is a shading gap,
not a march gap; one fix does not serve both.**

## S1d-4.5 A CONFOUND IN EVERY LID-vs-TESSELLATION COMPARISON

`--greets_displace_amp` defaults to **0.3 world** (`FeatureFlags.def:429`) while
the shell arm is pinned at `--pom_shell_world_amp_set=0.18`. **Every lid-vs-tess
comparison in this document and its predecessors compared a 0.18-world slab
against a 0.30-world carve — 1.67× different relief depth.** That affects the
see-through counts, the crisp-step comparison and the offscreen divergence
figure. It does not change §S1d-4.1 (a cap ladder against itself) or §S1d-4.3
(arm against arm at one amplitude), but every row with a `tessellation` cell in
it needs re-running at matched amplitude before it is quoted again.

## S1d-4.6 REPRODUCTION

```sh
# slip / reach ladder (scratchpad scripts)
sweepA.sh <outdir> $LID --pom_prism=1 --pom_shell_cap=<C>   # 16 frames, t=5958
slip.py   <outdir> "<label>"        # SLIP_HIST=1 adds the tail histogram

# relief depth, banded, masked to the stone
seethru.py <flatdir-with-uvgeo> <armdir> "<label>"

# path-class flips
sweepA.sh <outdir> $LID --pom_prism=1 --pom_shell_cap=16 --pom_path_viz=2
pathflip.py <outdir> "<label>"

# the wall's most edge-on band, where gap 2 lives
#   crop x820-1240 y900-1080 at t=5958 cam p10
```

---

# S1d-5 — WRONG NORMALS: the prism side quads are shaded with the LID's normal

Added 2026-08-07. User verdict on the lid+prism arm, on screen, verbatim:
*"B has gaps and it has definitly wrong normals. but it looks good and does the
work - including seeing behind relief (where it should)"* — and, localising it,
*"I think the normals issue is related to winding - it has discontiuity on
polygon edges that are two flat connected polys creating a quad."*

## S1d-5.1 THE TRIAGE — M1/M2/M3 are all CLEAN at the poses tested

Three vizzes, one render each, on the lid+prism arm at t=5743 and t=5958.
**`--no-hdr` is mandatory**: `GREETS.CPP:1177` forces `hdr` on for greets and the
viz stomps `outR/G/B`, which the HDR path discards — the first attempt at this
silently rendered an ordinary tonemapped wall and looked like "the viz does not
apply to the stone". `--viz_*` are `FDS_DEV`-gated, so this needs `build-dev`
(which does not overwrite `Runtime/DEMO`).

Also required: `--no-pom_normal`. `--pom_normal` perturbs the G-BUFFER normal in
the rasterizer, so `--viz_geonormal` still carries it and the "pre-normal-map"
reading is not pre-anything until `--pom_normal` is off.

| candidate | instrument | result at t=5743 |
|---|---|---|
| **M1** vertex-normal interpolation crease | `--viz_geonormal --no-pom_normal --wire_viz=2` | **CLEAN** — the wall is ONE solid colour, the quad's triangle diagonal is visible in the wire overlay, no kink across it |
| **M2** per-triangle tangent on the non-affine trapezoid chart | `--viz_tangent --no-pom_normal --wire_viz=2` | **CLEAN** — one solid colour, no break on the same diagonal |
| **M3** handedness split cutting through a quad | `--viz_matid --wire_viz=2` | **NOT CUTTING** — the whole wall panel is ONE material id across the diagonal; the split exists scene-wide (`[GREETS-TBN] split 2479 mirrored-UV faces onto 19 handedness clones`) but does not divide this quad |

So the diagonal-kink he described is **not reproduced at either pose I triaged**,
and I do not have a mechanism for it yet. That is a gap in this report, not a
refutation of what he saw — he was scrubbing a continuous fly, and I sampled two
static poses. **The next move on that defect is his pose, not another A/B.**

## S1d-5.2 WHAT IS CERTAIN — the prism side quads, in code and on screen

`DEMO/MeshOps.cpp:5951-5954`, `PomShell_BuildPrism`:

```cpp
qv.push_back({ Ta, va->N, va->Tangent, uA, vA, hA });
qv.push_back({ Tb, vb->N, vb->Tangent, uB, vB, hB });
qv.push_back({ Bb, vb->N, vb->Tangent, uB + kNudge*nu, vB + kNudge*nv, 1.0f - hB });
qv.push_back({ Ba, va->N, va->Tangent, uA + kNudge*nu, vA + kNudge*nv, 1.0f - hA });
```

Every one of the four side-quad vertices takes **`va->N` / `vb->N` — the LID
vertex's normal**. The quad is the SIDE of the relief slab, geometrically
perpendicular to the lid. Its true outward direction is already computed 29
lines earlier (`const Vector out = {…}`, line 5922) and is used **only to pick
the winding**, never as a shading normal.

Confirmed on screen at p5 t=5963 (where the prism strip is largest — it replaced
the 8 700 px gash), by differencing `--pom_prism=1` against `--pom_prism=0` under
`--viz_geonormal` to isolate exactly the 33 441 prism-owned pixels:

- **prism OFF**: three distinct normal regions — the left wall (teal), the right
  wall (orange), and a narrow band of background/junk showing through the slit.
- **prism ON**: the slit is gone and the strip is **pure teal — bit-for-bit the
  left wall's normal.** There is no distinct normal for the side face at all.

So the strip that seals the gash is lit as though it were a continuation of the
wall it came from. **This is a certain, arm-B-specific defect, it is the new
geometry from `S1d-3` (`e144205`), and it is by construction rather than by
accident.**

## S1d-5.3 THE DESIGN TENSION THE FIX HAS TO RESOLVE

`Vertex::N` on a prism side has two consumers that want different values:

- the **deferred kernel's shading normal** wants the side's own `out`;
- the **march** wants the LID's `N`, because `VtN = V·N` parametrises a ray that
  is supposed to continue the neighbouring lid's relief, in the lid's tangent
  frame and UV domain (which is why `S1d-3.0` chose the lid's basis in the first
  place — it is not an oversight, it is a documented trade).

So this is not a one-line sign fix. The options are (i) give the side its true
normal and accept that the march on a side fragment is degenerate anyway — its
UV is constant down the quad apart from a 1e-4 nudge, so it is carrying almost
no relief; (ii) keep the lid basis for the march and route a separate shading
normal for side faces; (iii) suppress the march on side quads entirely and shade
them as plain textured sides. **(i) is the cheapest and is the one I would
measure first.** Not implemented in this stage.

## S1d-5.4 TWO ORDERING FACTS, CONFIRMED

- `GreetsFixBitangentHandedness` runs at `GREETS.CPP:2643`, **after**
  `PomShell_Build` (:1911) but **before** `PomShell_BuildPrism` (:2653). Lid
  faces therefore get the handedness split and **prism sides are created after
  it, so they never see it** — they inherit whichever clone their owner landed
  on.
- A side quad is perpendicular to its owner, so its own UV determinant sign has
  no reason to match the owner's. Inheriting the owner's `TbnHandedness` is
  right only by luck. Both facts are consistent with `S1d-5.2` and neither has
  been measured for its pixel cost yet.

---

# S1d-5b — THE PRISM-CLIPPED MARCH (`--pom_prism_march`), built and measured

Added 2026-08-07, commits `eabb28e` (stage 1) + `de29caa` (stage 2). The user's
mandate: replace the per-patch UV-box march domain with a PRISM-CLIPPED march
per the literature (Hirche 2004, Shell Maps) — the ray bounded to the prism,
marched entry→exit, DISCARDED on exit, the neighbouring prism's own fragment
answering the pixel. This section records what of that survived contact with a
front-face rasterizer, all of it measured.

**Metric note.** All void/black numbers below are MY OWN baseline re-renders on
one binary: void = `popcount(z16==0)`, black = `popcount(max(r,g,b)==0)`, over
the **19-pose set** = the 16 lines of `docs/greets_review_poses.txt` + t=2980 +
t=5518 + t=5877 (the S1d-4 mandate extras). The c5bf8ec per-pose figures (14 /
1+9 / 15) came from dumps that no longer exist and do not reproduce from the
stored PPMs; the flat-quad arm re-measured on this metric is the 106 682 row
below, and every comparison in this section is same-binary, same-metric.

## S1d-5b.0 The mode ladder

`--pom_prism_march` (default 0, byte-null; needs `--pom_shell --pom_prism`,
cone march). **Mode 1 is the recommendation** — stage 2 named mode 2 the
production candidate, and §S1d-5b.7 overturned that on measurement; modes 2 and
3 remain in the tree as measured references.

- **1 — SIDE-QUAD ENTRY MARCHING. ← the arm to fly (§S1d-5b.10).** The prism quads march the owner's height
  field from their own interpolated (u,v,h) (`Vertex::ShellH` already runs
  lid→base down the quad). Three mechanisms, each necessary:
  (a) per-lane SIGNED `1/(V·N)` on side quads ONLY (`Face::PomPrismSide`) — a
  side fragment can be seen from behind the owner lid's plane (V·N < 0, the
  t=5963 doorway curtain) where the ray ASCENDS; ascent cone step is
  `c·gap/(dlen−c)`, `dlen≤c` ⇒ exits the lid ⇒ MISS; ascent miss is `rayH≥1`.
  Applied to LID faces this discards 200k px over the 19 poses (grazing lanes
  whose interpolated V·N dips ≤0 flip to ascent and exit) — the side-only gate
  is load-bearing.
  (b) the 1e-4 UV nudge points INTO the owner's triangle (the legacy
  handedness-matched sign can point outside and turn every in-stone
  hit-at-entry into a lateral exit).
  (c) owner material + owner lid N/Tangent (march frame = owner chart); under
  `--pom_normal` a HIT shades with the marched field's own normal — which
  retires §S1d-5.2's wrong-normals defect for every crossing ray.
- **2 — FULL INTERFACE GEOMETRY** (stage 2's candidate, SUPERSEDED by mode 1 —
  §S1d-5b.7: it ties on the 19 poses, loses 2× on the authored timeline, and
  costs +4.38 ms for 4.09× the side faces), exit policy
  unchanged (lid_edge etc. still apply):
  * CHART-TEAR walls: watertight now also requires the partner to CONTINUE THE
    OWNER'S CHART (same material — `::mirUV` clone = tear — coplanar, equal
    endpoint UVs). Welded folds get their interface walls back (+228 @ weld=1).
  * T-JUNCTION SPLIT (§8.6 5b): emitted walls split at interior T-vertices
    with the WELDED delta, keyed on `PomWeldQPos` both sides. Census: 17 edges
    / 40 T-points, 30–40 NON-AFFINE, max 0.127 world — the affinity is
    measurably violated, the split is load-bearing.
  * TWO-SIDED interface walls (partnered edges only): at a concave welded fold
    both owners' walls face INTO the solid and cull away; the reversed copy is
    the neighbour-chart continuation entry, the wrong-chart copy self-discards.
    Free edges stay one-sided (see-through by design).
- **3 — THE PAPER-PURE EXIT** (reference): every failure discards; base clip /
  lid-edge clamps / keep_uv / side planes / side entry forced off; plus a
  coplanar-exit keep (crossed hit whose exit side is `shellSideCls==0`).

## S1d-5b.1 THE HEADLINE TABLE — 19 poses, void / black

> **MIP-CONDITION NOTE (S1d-8, 2026-08-08): every number in this section was measured with `--mips` OFF, which is no longer the default.** Re-measured under the flip in **§S1d-8.1**: the arm's **void is mip-INVARIANT** (10 at both settings, same three poses, same counts), so every void comparison here stands. **BLACK does not**: it falls ~25 % (73→52 on the arm, 139→120 on flat) because coarser mips average pure-black stone texels away. Do not quote a black figure from this section against a post-flip render.

| arm (cap=16, amp=0.18, prism=1) | void | black |
|---|--:|--:|
| S1d-4 shipping arm (flat quads, weld=0, lid_edge=3) | 106 682 | 106 743 |
| marching quads, flat=0 (same flags) | 20 | 84 |
| mode 1, weld=0 (stage 1) | 20 | 85 |
| mode 3, weld=0 (paper-pure) | 17 871 | 13 646 |
| mode 3, weld=1 | 20 210 | 17 179 |
| mode 3, weld=3 | 12 317 | 9 878 |
| mode 2, weld=1, lid_edge=3 (the stage-2 candidate) | 10 | 84 |
| mode 2, weld=3, lid_edge=3 | 476 | 531 |
| **mode 1, weld=1, lid_edge=3 — THE ARM TO FLY** (§S1d-5b.7) | **10** | **84** |
| flat, no displacement at all — the crack FLOOR | 21 | 148 |

**The last two rows were added 2026-08-07 and they change the conclusion.** This
table cannot separate mode 1 + weld=1 from mode 2 — they tie at 10 / 84 — and it
is bounded below by an engine crack floor of 21 that a flat scene already has.
The arms are told apart by the AUTHORED-CAMERA TIMELINE (§S1d-5b.5) and by PERF
(§S1d-5b.6), and on both mode 1 wins. Do not pick an arm off this table alone.

Two rows deserve their own sentence:

1. **The S1d-4 flat quads were costing 106k px of Z-coverage at three poses
   c5bf8ec never measured**: p5963 91 030 (the OPEN defect), and the two
   MIRROR poses p6133 11 507 / p6293 4 104. All three are 0 under every
   marching arm — the t=5963 forward-sentinel void (stage 4 of the mandate)
   is RESOLVED BY ARCHITECTURE: the flat quads are gone, so is the defect,
   0 void / 0 black at the pose, no GreetsMirror work needed.
2. **The paper-pure exit loses by three orders of magnitude** (12 317 vs 10),
   and §S1d-5b.2 is the mechanism, measured.

Candidate vs the S1d-4 acceptance poses: p5518 11→7, p2980 0→0, p5958b 0→0,
p2845/p6097 0→0, p5534 4→0, p5877 5→1 with the wall-top crenellation
PIXEL-IDENTICAL to the mode-1 reference (see-through preserved) and the
near-wall corner rendering a relieved marched jamb instead of a razor cut.
LOOK at t=5963: the doorway curtain is marched stone continuous with the wall
relief (stage-1 goal; scratch `st1_curtain_5963.png`) — the flat quads showed
a stretched texel band, flat=0 erased the jamb to a see-through sliver.
Weld re-derived for this architecture: **weld=1**. weld=3 regresses (476 —
new slits at p5743/5773 where wall/floor junction verts move differently);
weld=0 tears (222 torn edges vs 66; non-affine T max 0.127 vs 0.100).
Candidate byte-stable 3/3 at t=5743; every shipping gate byte-exact and the
S1d-4 arm with the flag unset byte-MATCHES the pre-S1d-5 binary.

## S1d-5b.2 WHERE THE ARCHITECTURE FIGHTS THE ENGINE — the finding

**"Discard on exit, the neighbour answers" requires a fragment per crossed
prism. A front-face rasterizer cannot produce one for the seam classes that
dominate greets' void.** Proof by exhaustion at t=2980 (the corner pose, a
3 588-px full-height seam slit, exits U−/U+ into each other):

| experiment | void |
|---|--:|
| mode 3, walls only where the geometric rule emits | 3 588 |
| + `--pom_prism=2` (walls at EVERY edge) | 3 588 |
| + two-sided walls | 3 538 |
| + walls NEVER domain-discard (temp build) | 3 362 |

Walls everywhere, facing both ways, never discarding — the slit stays,
because the interface sheet between two COPLANAR prisms contains the view ray
at every front-on pose: its projected area is a razor sliver (the S1d-4 keep
arm renders the same seam as a seamless continuous wall — the crossed hits ARE
correct there). Hirche gets his fragment-per-crossed-prism from
projected-tetrahedra SOLID rasterization (the prism's projected silhouette
covers the pixel whenever the ray crosses the volume); Shell Maps never have
the boundary at all (one continuous chart). Our per-patch boxes manufacture
boundaries neither paper has, and the only continuation available at them is
the MARCH's own crossed hit — i.e. exactly what `--pom_shell_lid_edge=3` +
`--pom_shell_keep_uv=0.125` keep, bounded to half a block. The path census
agrees: the mode-3 void is 94 % `coneHit + DISCARD(domain)` with the kill
sticky bit set — the march FOUND the stone and the policy threw it away, at
pixels where no answering geometry can exist.

The per-side-class coplanar keep built into mode 3 does not rescue it
(2980: 3 538 unchanged) — `shellSideCls` is the DOMINANT class per patch-box
side, and the offending seams sit on sides whose dominant class is angled;
S1d-2b hit the same coarseness on free edges.

So the honest composition is: **prism walls as marching ENTRY geometry +
bounded march continuation at the rasterizer-unanswerable seams.** The exact
flags to fly are §S1d-5b.8, which is shorter than the arm measured here —
two of its flags turned out to be measurably droppable.

## S1d-5b.3 STAGE 3 (coverage) — the residual, attributed

**§8.6 req 4 ("shell `siling`, the columns and the stairs — 30 % of the wall's
concave boundary abuts unshelled geometry") is measured to buy ZERO here, and
was NOT done.** The attribution, not the magnitude, is the argument.

The candidate's entire 19-pose residual is 10 void px: 7 @ p5518, 2 @ p5773,
1 @ p5877. Each one was attributed three ways (`--face_id_dump` +
`--pom_path_viz=2`, scripts `attrib.py` / `attrib_black.py`):

1. **Neighbourhood.** All 10 are ISOLATED SINGLE PIXELS whose ENTIRE 8-neighbour
   ring is ONE shelled material — `floor::mirUV` ×6, `rooms` ×3, `rooms::mirUV`
   ×1. Not one touches `siling`, `teleporter`, a column or a stair. Not one even
   touches a material BOUNDARY. Shelling the unshelled 30 % cannot reach a single
   one of them.
2. **Path code.** All 10 read `path == 0` — **no fragment was ever rasterized
   there**. They are rasterizer sliver cracks between two abutting triangles
   interior to one chart, not march discards and not chart-boundary gaps. No exit
   policy, weld setting or interface wall can address a pixel no triangle covered.
3. **Against the floor.** A FLAT arm (`--deferred` alone, no displacement of any
   kind) shows **21** such holes at the same 19 poses. The candidate's 10 is
   BELOW the undisplaced scene's own crack rate, so the metric has bottomed out
   against the engine, not against the shell.

The BLACK residual (84) decomposes just as cleanly: 10 void-black + **74
shaded-black, every one of them owned by `Piramid.lwo`'s `sss` material** —
unshelled, unrelated to the stone, and present in the flat arm too (flat shows
62 of them at p5958a where the candidate shows 60). Flat's total black over the
19 poses is **148** against the candidate's **84**.

**But the 19 poses were not enough to close coverage — see §S1d-5b.5.** They are
hand-picked defect repros; a census on the DEMO's OWN authored camera finds two
t-ranges they never sample where mode 2 leaves ~110 and ~205 void px/frame. That
residual is also not the unshelled 30 % (it is `rooms::mirUV`), and chasing it
is what overturned the mode-2 candidate in §S1d-5b.7.

## S1d-5b.4 MATCHED AMPLITUDE — the §S1d-4.5 confound, discharged

§S1d-4.5 flagged that every lid-vs-tessellation cell in this campaign compared a
0.18-world slab against `--greets_displace_amp`'s 0.30-world carve. Re-run at ONE
amplitude, 19 poses, same binary, void / black:

| arm | void | black |
|---|--:|--:|
| flat (`--deferred` alone, no displacement) | 21 | 148 |
| tessellation @ **0.30** (the confounded number) | **271** | **444** |
| tessellation @ **0.18** (matched to the shell) | **20** | **192** |
| shell, mode 2 @ 0.18 | 10 | 84 |
| shell, mode 1 + weld=1 @ 0.18 (§S1d-5b.7) | 10 | 84 |

**The confound was worth 13.6× on the tessellation arm** (271 → 20 void going
from 0.30 to 0.18). Every historical cell that ran tess at its default was
flattering the shell by an order of magnitude. At matched amplitude the shell
still wins — **10 vs 20 void, 84 vs 192 black** — but by **2×, not 27×**. That is
the number to quote from now on; the old one is retired.

Note also that flat (no displacement at all) scores 21 / 148. The tessellation
bake at matched amplitude is therefore roughly AT the undisplaced scene's own
crack floor on void, and worse than it on black.

## S1d-5b.5 WHAT THE 19 REVIEW POSES DO NOT COVER

`docs/greets_review_poses.txt` is a list of poses **defects were reported at**.
That makes it a biased sample by construction: it can only find defects where
someone already looked. Every entry sits at t≈2845–6293, sixteen of nineteen at
t≥5518.

**Instrument (new, `st3_timeline.py`).** Walk the greets timeline on the DEMO's
OWN AUTHORED CAMERA — `FDS_GREETS_CAM` explicitly UNSET — every 100 ticks from
t=200 to t=6400 (63 samples), scoring the same void/black. This is the camera the
user actually flies, so a defect CLASS the review list misses shows up as a
t-range with residual.

It found two, and neither is sampled by any review pose:

| band | flat | flat+POM | S1d-4 | mode 2 | **mode 1 + weld=1** |
|---|--:|--:|--:|--:|--:|
| t=900…2400 (6 samples) | 0–3 | 0–3 | 386–2585 | **101–129** | **0** |
| t=3500…4000 (6 samples) | 0–1 | 0 | 8079–8126 | **198–217** | **0–1** |

Two things follow, both measured:

1. **Flat POM is identical to flat** — the void in these bands is entirely the
   SHELL's, not POM's.
2. **Mode 2 improves S1d-4 enormously here** (t=3500: 8079 → 217, 37×) **and
   still leaves ~110–205 px/frame that the undisplaced scene does not have.**
   The 19-pose census reports 10 because it has no sample in either range.

Attribution of the band residual (`--face_id_dump` + `--pom_path_viz=2` at
t=1500/3500/3800, 531 void px): **99 % of the neighbour ring is `rooms::mirUV`** —
the handedness-mirrored wall clone — and `siling` contributes 6 of 794 samples
(0.75 %). So §8.6 req 4 is refuted here too. The pixels are ~98 % `path == 0`
(no fragment rasterized) and form a coherent thin seam ~115 rows tall, ~2 px per
row, not scattered pinholes.

**Standing lesson: a pose-list census cannot certify coverage.** Run the
authored-camera timeline before claiming a coverage stage is closed.

## S1d-5b.6 PERF at t=5743 — and the 5 ms is not where it looked

> **MIP-CONDITION NOTE (S1d-8, 2026-08-08): every number in this section was measured with `--mips` OFF, which is no longer the default.** **SUPERSEDED by §S1d-8.4 on two rows.** (a) `--mips` ON is NOT perf-neutral for the parallax arms — it buys 0.6–0.7 ms on +POM, +tessellation and the shell arm (and only 0.1 on flat). (b) **the `mode 1 + weld=1` 56.98 vs `tessellation@0.18` 56.66 tie (+0.32 ms) DOES NOT REPRODUCE**: over 14 interleaved rounds the shell arm costs **+1.04 to +1.58 ms MORE than tessellation** at matched amplitude, at BOTH mip settings. `arm − flat` reproduces exactly (+7.09 fmin / +7.50 bench vs the published +7.44).

`--bench=scene@scene=greets,t=5743,iters=20`, 5 interleaved rounds, min-of-arm
(the only load-robust statistic; load averaged 7–11 from a concurrent agent, so
read every row as an upper bound). Both statistics recorded per run.

| arm | bench min ms | Δ flat | profiler min ms | Δ flat |
|---|--:|--:|--:|--:|
| flat | 49.54 | — | 48.05 | — |
| + POM | 55.57 | +6.03 | 53.04 | +4.99 |
| + tessellation @0.18 | 56.66 | +7.11 | 54.61 | +6.56 |
| + tessellation @0.30 | 56.34 | +6.80 | 54.71 | +6.66 |
| S1d-4 | 56.69 | +7.15 | 55.05 | +7.00 |
| **mode 1 + weld=1** | **56.98** | **+7.44** | — | — |
| mode 2 (the ex-candidate) | 61.86 | +12.32 | 59.68 | +11.63 |

**My flat reproduces the 49.25 the previous stage reported; its tess figure of
73.99 does NOT reproduce — I measure 56.66 at matched amplitude and 56.34 at
0.30, a 17 ms gap I cannot account for.** That triple appears nowhere in the
repo, so it cannot be traced. Treat the row above as the live one.

On this harness **mode 2 costs 5.2 ms MORE than the tessellation carve it was
meant to replace.** That inverts the campaign's cost argument and is not a
rounding error, so it was decomposed rather than described.

**Mechanism bench** (same recipe, 5 rounds, arms differing one flag at a time;
`faces` is the `pom_prism_sides` mesh's face count from the run's own log):

| arm | min ms | side faces | Δ prev | Δ S1d-4 |
|---|--:|--:|--:|--:|
| S1d-4 (flat quads, weld=0) | 56.76 | 530 | — | — |
| flat quads, weld=1 | 57.16 | 218 | +0.39 | +0.39 |
| **mode 1 — quads MARCH** | 56.98 | 392 | **−0.18** | +0.22 |
| **mode 2 — + interface geometry** | 61.36 | **1602** | **+4.38** | +4.60 |

**Turning the side quads into marchers is FREE (−0.18 ms, inside noise). The
entire cost is mode 2's added interface geometry — 392 → 1602 faces, 4.09× —
from the 228 chart-tear walls, the two-sided reversed copies and the T-splits.**

## S1d-5b.7 THE VERDICT REVERSAL — mode 1 + weld=1 dominates mode 2

§S1d-5b.6 forced the one combination stage 2 never ran. Its ladder varied MODE
and WELD together (`mode 1 weld=0`, `mode 2 weld=1`, `mode 3 weld=0/1/3`) and so
never measured **mode 1 with weld=1** — the cheap corner of the grid.

| arm | 19 poses void/black | authored timeline, 63 steps | t=5743 ms |
|---|--:|--:|--:|
| flat | 21 / 148 [S1d-8: re-measures **18 / 146**, unattributed — see §S1d-8.0] | 5 332 | 49.54 |
| S1d-4 | 106 682 / 106 743 | — | 56.69 |
| mode 2 (ex-candidate) | 10 / 84 | **7 271** | 61.86 |
| **mode 1 + weld=1** | **10 / 84** | **3 575** | **56.98** |

**Mode 1 + weld=1 ties mode 2 on the 19 poses, beats it 2.0× on the authored
timeline (and comes in BELOW the undisplaced flat arm's 5 332), and costs
4.88 ms less.** Step by step across all 63 samples there is **no t where mode 2
beats mode 1 by more than 20 px**, and none where mode 1 is worse than flat by
more than 20 px.

So mode 2's interface geometry is not merely expensive — **it is the source of
the band residual it was built to remove.** The chart-tear walls, the two-sided
copies and the T-splits add 1 210 faces of `rooms::mirUV` interface sheet, and
that sheet is what leaves the ~115-row seam at t=3500–4000.

This does NOT retract §S1d-5b.2's architecture finding, which stands unchanged:
the paper-pure exit (mode 3) is still unimplementable in a front-face rasterizer,
and the bounded march continuation (`lid_edge=3` + `keep_uv`) is still what
answers the coplanar seams. What is retracted is the claim that mode 2's extra
GEOMETRY is needed on top of it. It is not; it is a net negative.

**`--pom_prism_march=2` and `=3` stay in the tree as measured references.** Mode
2 is no longer the recommendation.

## S1d-5b.8 THE GRAZING SMEAR — S1d-5 does not touch it

> **MIP-CONDITION NOTE (S1d-8, 2026-08-08): every number in this section was measured with `--mips` OFF, which is no longer the default.** **Re-measured and CONFIRMED UNCHANGED (§S1d-8.2): the flip does not move slip by 0.1 % at any cap**, and `--texture_filter=2` does not move it either — because `slip` is a MARCHED-UV metric and the albedo filter runs downstream of the march, so a slip ladder is blind to filtering by construction. The ladder below is therefore still live; what is retired is the expectation that a sampling change could show up in it.

Sweep A (t=5958, p9→p10, 16 frames, `FDS_DUMP_TXTR=1`), `slip.py`, against the
§S1d-4.1 published ladder. Slip p90 is the comparable column.

| arm | slip p50 | **slip p90** | slip p99 | reach p90 | published p90 |
|---|--:|--:|--:|--:|--:|
| flat POM | 0.007 | **0.094** | 0.57 | 10.28 | 0.094 |
| mode 1, cap 4 | 0.097 | **0.891** | 2.69 | 51.13 | 0.891 |
| mode 1, cap 16 | 0.239 | **6.271** | 29.47 | 182.50 | 6.246 |
| mode 2, cap 16 | 0.235 | **6.375** | 29.28 | 182.36 | 6.246 |
| mode 2, cap 4 | 0.097 | **0.891** | 2.69 | 51.10 | 0.891 |
| S1d-4, cap 16 | 0.252 | **6.658** | 30.43 | 185.35 | 6.246 |

**Stated plainly: S1d-5 DOES NOT MOVE THE SMEAR, in either mode.** flat and cap 4
reproduce the published ladder to three decimals (0.094, 0.891); cap 16 lands at
6.271 / 6.375 against 6.246, inside the spread of my own S1d-4 re-measure (6.658).

This is the expected result, not a failure. The smear is produced by the
cap-bounded `1/(V·N)` reach on the LID march. S1d-5 changes what happens at PRISM
BOUNDARIES — entry geometry and exit policy — and touches neither the lid march
nor the cap. **The grazing smear remains open and is untouched by this stage.**

## S1d-5b.9 CAP SENSITIVITY — the cap is free for coverage

> **MIP-CONDITION NOTE (S1d-8, 2026-08-08): every number in this section was measured with `--mips` OFF, which is no longer the default.** Still true post-flip: void is 10 at every cap at BOTH settings (§S1d-8.1/§S1d-8.2).

S1d-3 found the prism made `--pom_shell_cap` a free parameter. Confirmed for
this arm, 19 poses:

| cap | 2 | 4 | 8 | 16 |
|---|--:|--:|--:|--:|
| mode 2 void / black | 10 / 83 | 10 / 84 | 10 / 84 | 10 / 84 |
| mode 1 + weld=1 void / black | — | **10 / 84** | — | **10 / 84** |

**Void is exactly 10 at every cap. `--pom_shell_cap=16` is NOT load-bearing** —
the user can fly cap 4 at zero coverage cost. What the cap buys is REACH and what
it costs is SLIP (§S1d-5b.8): cap 4 → reach p90 51, slip p90 0.891; cap 16 →
reach p90 182 (3.6×), slip p90 6.27 (7.0×). Per §S1d-4.1 there is no knee on that
curve, so the choice is a look judgement, not an optimum. Cap 4 is recommended
because it is what the user flies and it is 7× quieter in motion.

## S1d-5b.10 THE ARM TO FLY

```sh
cd Runtime && ./DEMO \
  --deferred --pom_shell --pom_prism=1 --pom_prism_march=1 \
  --pom_shell_lid_edge=3 --no-pom_shell_base_clip \
  --pom_shell_world_amp --pom_shell_world_amp_set=0.18 \
  --pom_normal --parallax_pom_cone --parallax_pom=32 \
  --pom_cone_exact=1 --pom_cone_min_step=1 --pom_march_earlyout \
  --pom_shell_cap=4 --texture_filter=1
```

Measured **with `--mips` OFF**: **19 poses 10 void / 73 black; authored-camera bands t=900–2400 and t=3500–4000 both 0 void; 56.98 ms at t=5743**. **POST-FLIP (§S1d-8): 10 void / 52 black; 55.81 ms** (fmin min, 14 rounds) — void identical, black is a metric artefact of the sampling change, and the arm is now measured **+1.04 ms MORE than tessellation**, not level with it (+7.44 vs flat, +0.29 vs S1d-4,
+0.32 vs tessellation at matched amplitude).

**What was dropped, and why — each measured, not assumed:**

| dropped | reason |
|---|---|
| `--pom_shell_side_faces=3` | **inert.** Removing it is BYTE-IDENTICAL at t=5743/5963/2980. Confirms §8.6's "superseded". |
| `--pom_shell_weld=1` | already the DEFAULT (`FeatureFlags.def:204`). Byte-identical when omitted. |
| `--no-greets_displace` | already the default (`greets_displace` = 0). |
| `--pom_prism_march=2` → `=1` | §S1d-5b.7: mode 1 ties on poses, wins 2× on the timeline, costs 4.88 ms less. |

**What was KEPT because dropping it measurably hurt:**

- `--no-pom_shell_base_clip` is **load-bearing**: restoring the default adds
  **+43 black** on mode 1 (and +26 on mode 2, concentrated at the grazing poses
  p5958b/c). Keep it off.
- `--texture_filter=1` is a LOOK flag, orthogonal to coverage: void unchanged at
  10, black 84 → 73 on the poses. **The black metric is not comparable across a
  texture-filter change** — `flat --texture_filter=1` alone moves t=1500 black
  from 2 501 to 3 260, i.e. the shift is 100 % the filter. Drop the flag if you
  want black numbers comparable to the rest of this document.

**The one knob to try:** `--pom_shell_cap=16` for 3.6× deeper relief at 7×
the grazing smear. Coverage is identical (§S1d-5b.9).

Interactive: F1/F2 scrub (Shift = fast), F9 dumps a pose.

**Where to look, in priority order** — the two bands no review pose covers and
where mode 2 failed, so they are the least-eyeballed stretches of the scene:
**t≈900–2400** and **t≈3500–4000**, both on the authored camera. Then the
review poses themselves. The residual that remains is (a) 10 single-pixel
rasterizer cracks, below the flat arm's own 21, and (b) the grazing smear, which
this stage does not touch.

## S1d-5b.11 REPRODUCTION

Scripts in the session scratchpad (they are not in `tools/`):

```sh
# 19-pose battery + score  (void = popcount(z16==0), black = popcount(max rgb==0))
s1d5_poses.py <root> <armname> -- <flags...>      # needs FDS_SNAPSHOT_ZDUMP, sets it
s1d5_score.py <root> <arm> [<arm>...]

# THE AUTHORED-CAMERA TIMELINE CENSUS (S1d-5b.5) — unsets FDS_GREETS_CAM
st3_timeline.py <root> <arm> <t0> <t1> <step> -- <flags...>

# residual attribution: needs --face_id_dump (+ --pom_path_viz=2 for path codes)
attrib.py       <posedir>...    # void px -> neighbour ring material, SHELLED or not
attrib_black.py <posedir>...    # splits void-black from shaded-black, names the owner

# perf: interleaved, min-of-arm, records bench mean AND profiler min
st3_bench.sh                    # the S1d-5b.6 ladder
st3_mech.sh                     # the mechanism decomposition (faces vs marching)

# slip / reach (grazing smear)
sweepA.sh <ABSOLUTE outdir> <flags...>   # 16 frames t=5958; RELATIVE paths land in Runtime/
slip.py   <outdir> "<label>"
```

Gates, all re-run at this commit and byte-exact: `tools/render_gate.sh` 3/3
(`4ac809e5` / `b41894f9` / `166fa25a`), city `37e62845c4d30eefa321730c5bb7e0b8`,
fountain `51fff7cd38767d619280afe0498a6f24`, greets pin
`f1297141611c484bac7cc10a8bdcf630`.

---

# S1d-6 — THE SILHOUETTE IS A STRAIGHT LINE: the defect every census was blind to

Added 2026-08-08. User report: at t=5877 the shell arm's near-wall LEFT
SILHOUETTE is *"no see-through the wall edge — compare with the same pose with
tessellation."* Reproduced in `--snapshot`, root-caused, and **not yet fixed**:
four candidate rules were built and measured, all four are recorded below with
their numbers, and none is shippable. Read §S1d-6.5 before trying a fifth.

## S1d-6.1 WHY EVERY CENSUS MISSED IT — the methodological finding

> **MIP-CONDITION NOTE (S1d-8, 2026-08-08): every number in this section was measured with `--mips` OFF, which is no longer the default.** **Re-measured post-flip in §S1d-8.3 and BYTE-FOR-BYTE UNCHANGED** — every column of the four-arm table below is identical at both mip settings, because the metric reads the `z16` plane and mips change sampling, not where the lid projects. §S1d-6 stands in full.

**The void/black metric family cannot see this defect, structurally.** `void` is
`popcount(z16==0)`, `black` is `popcount(max(r,g,b)==0)`. Silhouette
see-through — background visible BETWEEN protruding blocks at a wall's screen
edge — is *lit background at a plausible depth*: neither zero-z nor black. A
perfectly straight wall edge scores flawless on both.

**And the one claim that did name this pose compared the wrong pair.** §S1d-5b.1
records "p5877 5→1, wall-top crenellation PIXEL-IDENTICAL to the mode-1
reference (see-through preserved)". That was shell mode 2 against shell mode 1 —
**two arms carrying the same defect**. It was never compared against
TESSELLATION, which is what the user's eye compares against. The same sentence
appears in `--pom_shell_lid_edge`'s own flag doc ("measured at t=5877 the
crenellated silhouette survives"); it is now measured false.

`tools/greets_silhouette.py` is the instrument that is not blind to it. The
authored wall edge is a straight 3D segment, so it projects to a straight screen
line; the metric traces the first NEAR pixel per row off the `z16` dump and
reports the residual about a Theil-Sen fit. **Validated on the three arms whose
answer is known by eye** (t=5877, cam
`15.5497618,3.4823668,-59.5607719,-0.524191499,-0.0974417627,0.846008122`,
**1920×1080** — `rev.cfg` unchanged; band y 250..860, x 880..1120, thresh 62000):

| arm | med x | off vs flat | **std** | p95 | rng | area | tv−net |
|---|--:|--:|--:|--:|--:|--:|--:|
| flat, no displacement | 978.0 | 0.0 | **0.46** | 1.00 | 1 | 188 | 0 |
| tessellation @0.18 | 983.0 | +5.0 | **2.43** | 5.00 | 17 | 1045 | 42 |
| tessellation @0.30 | 985.0 | +7.0 | **4.00** | 8.27 | 27 | 1696 | 66 |
| **shell, the §S1d-5b.10 arm** | **950.0** | **−28.0** | **0.35** | 1.00 | 1 | 85 | 0 |

Flat and tessellation separate 5.3× on `std`. **The shell arm scores BELOW flat
— it is straighter than the undisplaced wall — and sits 28 px OUTSIDE the
authored footprint.** Tessellation's silhouette only ever cuts INWARD (+5) because
the bake pins its patch-border verts to zero displacement; the shell's runs
outward, and straight.

## S1d-6.2 ROOT CAUSE — measured, from the path-viz plane

`--pom_path_viz=2` over the 28-px overhang band (17 080 px sampled, dev build,
`--no-hdr`):

| arm | dominant code | n |
|---|---|--:|
| the §S1d-5b.10 arm | `coneHit / action=KEEP / why=domain / exit=uMin` | 10 719 |
| + `--pom_shell_base_clip` | `coneHit / action=KEEP / why=domain+baseClip / exit=uMin` | 10 715 |
| + `--pom_shell_keep_uv=0` | `coneHit / action=DISCARD / why=domain+baseClip` | 1 450 |

The chain, each link measured:

1. `PomShell_Build` offsets every lid vertex by `worldAmp/2` along N, so at a
   patch border **the lid covers screen the authored plane never did** — here
   **+20 307 px** of near-wall coverage vs flat. The silhouette offset is
   **exactly linear in amplitude: −14 / −28 / −56 px at
   `--pom_shell_world_amp_set` 0.09 / 0.18 / 0.36.**
2. Rays entering that band march inward, leave the UV box through `uMin`, and
   find stone in the **height map tiling past the wall's end**.
3. `--pom_shell_lid_edge=3`'s `keepHit` keeps every one of them. Its gate tests
   only `pomCrossed` and the `--pom_shell_keep_uv` overshoot — **it never asks
   whether the pixel is over authored wall at all** (`keepHit` does not read
   `baseOK`, by construction).

**The wall does not end at a straight line because the blocks end there. It ends
at a straight line because the LID does, and nothing kills the overhang.**

Two corollaries, both measured and both correcting the record:

- **`--pom_shell_base_clip` alone does NOT fix it.** Silhouette byte-identical
  with the clip on (std 0.35, offset −28) — `keepHit` overrides `baseOK`.
- **At `--pom_shell_lid_edge=3` the base clip is a NO-OP on the 19 review
  poses: 10 void / 73 black with it ON *or* OFF, same binary.** That retires
  §S1d-5b.10's "`--no-pom_shell_base_clip` is load-bearing: +43 black without
  it". It is not load-bearing in this arm; it is inert.

## S1d-6.3 THE FOUR CANDIDATE FIXES, ALL MEASURED, NONE SHIPPABLE

Both flags below default to OFF and byte-null; all four shipping gates are
byte-exact (`render_gate` 3/3 `4ac809e5`/`b41894f9`/`166fa25a`, city
`37e62845`, fountain `51fff7cd`). Baseline for the arm: **10 void / 73 black**
over the 19 poses, reproducing §S1d-5b.10 exactly.

| candidate | t=5877 std | t=5877 void/black | **19 poses void/black** |
|---|--:|--:|--:|
| the arm (the defect) | 0.35 | 1/1 | **10 / 73** |
| `--pom_shell_base_clip` restored | 0.35 | 1/1 | 10 / 73 (inert) |
| `--pom_shell_lid_true_edge=1\|2` (free edges) | 0.35 | 1/1 | — (never fires, see below) |
| `--pom_shell_lid_true_edge=4` (convex ridges) | **5.02** | 1/1 | **127 043 / 125 643** |
| `--pom_shell_base_clip --pom_shell_keep_uv_overhang=0` | **2.26** | 549/380 | **152 662 / 146 984** |
| ...+ cap-bound lanes exempted | 2.26 | 549/380 | **151 733 / 146 644** |

**`--pom_shell_lid_true_edge`** brings S1d-2b's per-class terminal action to the
LID (its `sideKill` lives inside the `--pom_recess_only` branch and the lid path
never ran it). It is a bitmask over the exited box side's boundary class. The
class that owns this silhouette is measured to be **CONVEX (angled-out)**, not
free: **only 2 of 268 'rooms' box sides carry a TRUE-BOUNDARY sub-interval at
all**, because the greets walls are authored as CLOSED BOXES and this silhouette
is an outside CORNER. Killing the convex exit restores the crenellation exactly
(std 0.35 → 5.02, and it is *not* `lid_edge=0` — 281 305 px apart) **but costs
127 k void over the 19 poses**: most convex ridges in greets are not silhouettes,
their partner wall is FRONT-facing and the shell legitimately continues past the
ridge. That is precisely the black gash §S1d-2b predicted.

**`--pom_shell_keep_uv_overhang`** is a second `keep_uv` used only for
base-clip-rejected lanes, so an overhang pixel keeps its hit only if the hit
landed INSIDE the patch's own box — a genuine block of *this* wall standing
proud. **At t=5877 this is the best silhouette any arm produces: std 2.26 /
p95 5.00 / area 1142 against tessellation@0.18's 2.43 / 5.00 / 1045 at MATCHED
world amplitude (0.18 both sides).** But over the 19 poses it costs 152 k void,
concentrated at the GRAZING poses (t=5743 60 668, the t=5958 family ~32 000,
t=5843 18 127, t=5854 14 087): the base clip walks the ray to the authored plane
with `1/(V·N)`, and once that is far along the surface the "overhang band" is
most of the wall rather than a border strip — the failure
`--pom_shell_base_clip_raw` already documents. **Exempting cap-bound lanes was
tried and does not separate them (152 662 → 151 733), so grazing-ness measured by
the cap is NOT the discriminator.**

## S1d-6.4 WHAT THE FIX HAS TO DISCRIMINATE

Both failures are the same shape: a rule that is correct at the t=5877 silhouette
fires on a much larger population that is *not* a silhouette. The two populations
to separate are

- **wall ends here** — the ray has left the solid, background must win, discard;
- **wall continues here** — a coplanar seam, a fold, or a convex ridge whose
  partner is front-facing; keep or clamp, a discard is a gash.

Neither the box side's baked CLASS nor the base clip's boolean is that
discriminator. The two untried candidates, neither verified:

1. **Bake the ridge partner's NORMAL per box side** and test it against the view
   at ctx-build time. A convex ridge is a silhouette exactly when its partner is
   BACKFACING; that is view-dependent, is one dot product per face per side, and
   is the quantity the class lookup is standing in for.
2. **Gate on the base clip's OVERSHOOT DISTANCE, not its boolean** — kill only
   lanes whose base-plane crossing lands just outside the box (a genuine border
   strip, ~the projected `worldAmp/2`) and keep the ones that land far outside
   (the grazing wraps). The quantity is already computed inside the base clip.

## S1d-6.5 STANDING LESSON

**A see-through claim measured against another arm of the same family is not
measured.** Every "see-through preserved" / "crenellation survives" line in the
S1d-4 and S1d-5 record that used a shell arm as its reference is
reference-relative and unproven; §S1d-6.6 lists them. Tessellation, or the flat
wall, or a metric that reads the silhouette directly, is the only admissible
reference — and the comparison must be at MATCHED WORLD AMPLITUDE.

## S1d-6.6 RE-AUDIT — which see-through conclusions are reference-relative

| claim | reference used | status |
|---|---|---|
| §S1d-5b.1 / stage 2 commit: "p5877 wall-top crenellation PIXEL-IDENTICAL to the mode-1 reference (see-through preserved)" | shell mode 1 | **REFUTED.** Both arms have std 0.35 vs flat 0.46. Identical because both are broken. |
| `--pom_shell_lid_edge` flag doc: "measured at t=5877 the crenellated silhouette survives" | shell arm | **REFUTED**, same measurement. |
| `--pom_shell_lid_edge` doc: "silhouette crenellation is BASE-CLIP discards (lid overhang, baseOK=false) ... keeping crossed hits cannot paint over them" | none — asserted | **REFUTED.** The overhang lanes DO cross (10 719 of 17 080 are `coneHit`), so `keepHit` paints over exactly them. |
| §S1d-5b.10: "`--no-pom_shell_base_clip` is load-bearing: +43 black without it" | shell arm, different binary | **NOT REPRODUCED.** 10/73 with the clip ON or OFF, 19 poses, same binary. |
| `--pom_prism_free` doc: "the see-through past the last block's silhouette ... user-confirmed correct at t=5877" | user eye, older arm | **STALE** for the current arm: `--pom_prism=0` is byte-identical at this silhouette (std 0.35), so side quads are not what covers it. |
| §S1d-5b.1: "the doorway curtain reads as marched stone continuous with the wall relief" (t=5963) | flat-quad shell arm | **UNPROVEN** — never compared to tessellation. Not re-measured here. |
| §S1d-1: "all 95 546 true-boundary px void ZERO under a discard" | void metric | **SOUND but narrow** — it is a void claim, and void cannot see silhouette see-through. It does not license the free-edge rule at t=5877, where only 2 of 268 sides are free. |

## S1d-6.7 REPRODUCTION

```sh
# the metric (validated on flat / tessellation / shell, §S1d-6.1)
FDS_SNAPSHOT_ZDUMP=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
FDS_GREETS_CAM="15.5497618,3.4823668,-59.5607719,-0.524191499,-0.0974417627,0.846008122" \
  ./DEMO --snapshot=greets@t=5877 --out=DIR <arm flags>
tools/greets_silhouette.py DIR_flat DIR_tess DIR_arm     # flat FIRST (the `off` datum)
tools/greets_silhouette.py --hist DIR_flat               # pick --thresh at a new pose
```

Tessellation at MATCHED amplitude is `--deferred --greets_displace
--greets_displace_amp=0.18 --texture_filter=1`; flat is `--deferred
--texture_filter=1`.

**Scripting warning, learned the hard way this session:** zsh does not
word-split an unquoted parameter expansion, so passing an arm as `$ARM` (or
through `eval`) hands DEMO ONE argv token, which it reports as
`[FLAGS] unknown flag '...'` and ignores — a whole render batch then silently
runs at DEFAULTS. Build every arm as a real array and expand it `"${ARM[@]}"`,
and assert `grep -c "unknown flag"` on every log. One round of this campaign's
evidence images was voided by exactly this.

---

# S1d-7 — THE QUAD-DIAGONAL CREASE: the entry-height kink is NOT the whole cause

Added 2026-08-08. Handed over root-caused by the interactive-repro agent
(`--repro`, `docs/INTERACTIVE_REPRO.md`): plain parallax has no crease, minimal
`--pom_shell` has it; `--wire_viz=1` shows the block is ONE QUAD, TWO TRIANGLES
and the crease lies EXACTLY on the triangulation diagonal; the proposed
mechanism was that `DEMO/MeshOps.cpp`'s per-vertex `ShellH = 0.5 + 0.5·ndv`
(the corner correction), transported as a per-vertex interpolant and used as
the march's ENTRY HEIGHT, is only C1 across the shared diagonal when
`h1 + h3 == h2 + h4`.

**Measured, and the mechanism is only PARTIAL.** `--pom_shell_entry_flat`
(S1d-7, default 0, byte-null) stamps `ShellH = 1` on every moved lid vertex,
removing the `ndv` dependence entirely — so if the crease is the entry-height
kink it must vanish. Rendered at **t=2993 on the authored camera**
(`FDS_GREETS_CAM` unset), 1920×1080, arm `--deferred --pom_shell
--pom_shell_world_amp --pom_shell_world_amp_set=0.18 --parallax_pom_cone
--parallax_pom=32 --pom_cone_exact=1 --pom_cone_min_step=1 --texture_filter=1`:

| arm | diagonal crease |
|---|---|
| plain POM, no shell | **absent** (confirms the repro agent's necessity result) |
| `--pom_shell` | **present**, sharp, exactly on the diagonal |
| `--pom_shell --pom_shell_entry_flat` | **STILL PRESENT**, attenuated but not gone |

The flag changes 1 052 463 px at this pose, so it is emphatically live and
`ndv < 1` is *widespread*, not a rare-corner effect — yet the crease survives it.

**The stronger candidate the A/B points at, not yet confirmed:** the vertices
still move by `off` along their SMOOTHED normals whatever `ShellH` is stamped,
so on an authored-flat quad whose four corners carry different `ndv` **the LID
QUAD IS NON-PLANAR**. `PomShell_Build`'s re-planing loop then re-derives `F.N`
and `F.NormProd` **per triangle** from that triangle's own three moved vertices,
so the two halves of one quad get genuinely DIFFERENT plane equations — and
every per-face quantity the march derives from the plane (the march frame, the
depth write's `Vz/(V·N)`, the tangent frame) steps across the diagonal. That
explains why removing the entry-height kink attenuates the crease without
removing it: `entry_flat` fixes one of two per-triangle discontinuities.

> **RESOLVED 2026-08-08 — §S1d-8.5. The census was run and the hypothesis is CONFIRMED**: on `rooms` the two halves of a quad carry lid normals a mean 0.659° / max 3.068° apart, with a plane gap up to **0.0878 world against a 0.0900 lid offset**, over 133 authored-coplanar pairs; `floor`, which has **zero** corner verts, measures exactly 0.0000° on all 24 — the effect exists exactly where the smoothed-normal corner correction does. **`--pom_shell_lid_planar` (new, default 0, byte-null) removes the crease** at t=2993 on both the minimal and the full arm, at zero cost on the 19 poses (10/52 and 10/73 unchanged), on the silhouette (identical) and on perf (inside the noise floor). **It does NOT fix the silhouette** — §S1d-6.4's hope that one fix serves both is measured false, because the overhang is made by where the vertices SIT and this flag moves none of them.

**Next step, unverified (the original text, kept for the record):** confirm by measuring the angle between the two
triangles' re-derived normals per target quad (a bake-time census) and
correlating it with where the crease is visible; then decide whether the lid
should be made PLANAR per authored quad (project the corner offsets onto the
authored plane's normal, losing the mitre) or whether the march should read the
AUTHORED plane rather than the re-derived lid plane. The latter is also what
§S1d-6.4 wants for the silhouette — both defects are about what the shell hands
the march at a face's boundary, so look for one fix before building two.

---

# S1d-8 — THE POST-MIP-FLIP BASELINE, and the quad-diagonal crease ROOT-CAUSED AND FIXED

Added 2026-08-08, after `--mips` and `--mip_fix` both went **default ON** (see
`docs/SESSION_STATE.md`, 2026-08-08). **Every number in S1d-1 … S1d-7 was measured
with `--mips` OFF, i.e. every texture — albedo, normal, roughness, metal, AO *and
the height/cone maps the march samples* — pinned at level 0.** The height gathers
take the face's albedo miplevel unless `--pom_height_mip` pins them, so in
principle the flip changes the surface the march walks. This section re-establishes
the baseline. **Every figure is reported as a PAIR:** `--no-mips --no-mip_fix`
(reproducing the published number as a control) and the new default.

**Measurement isolation.** All of it was run on a private `git worktree` built from
`97b13fd` (`/Users/gil-ad/work/rev-s1d8`), executed with `cwd=Runtime/` of the main
tree so it sees the user's uncommitted authoring assets, so four concurrent agents'
in-flight edits to `FDS/RENDER/**` and `DEMO/*` cannot contaminate a single figure.
That binary reproduces the greets pin **`adfba8ba`** (mips ON) and **`f1297141`**
(`--no-mips --no-mip_fix`) byte-exact, and `render_gate` 3/3.

## S1d-8.0 THE CONTROLS — what reproduced and what did not

| control | published | mine, `--no-mips --no-mip_fix` | verdict |
|---|---|---|---|
| the §S1d-5b.10 arm, 19 poses | 10 void / 73 black | **10 / 73**, and per-pose **7@p5518 + 2@p5773 + 1@p5877** — the §S1d-5b.3 residual breakdown to the digit | **EXACT** |
| slip p90, flat POM | 0.094 | **0.0944** (p50 0.0072, p99 0.5731 vs published 0.007 / 0.57) | **EXACT** |
| slip p90, cap 2 / 4 / 8 / 16 | 0.254 / 0.891 / 2.530 / 6.271 | **0.2541 / 0.8910 / 2.5379 / 6.2705** | **EXACT** |
| silhouette t=5877, flat / tess@0.18 / shell | 0.46 / 2.43 / 0.35, off 0 / +5 / −28 | **0.46 / 2.43 / 0.35, off 0 / +5 / −28**, every column identical | **EXACT** |
| flat arm, 19 poses | 21 / 148 | **18 / 146** (bare `--deferred`); **18 / 139** with `--texture_filter=1`; **18 / 139** for flat+POM | **3 void / 2 black SHORT — not attributed** |

**The flat control is the one gap, and it is stated rather than papered over.**
Three different flat flag-sets all give exactly 18 void, never 21, while the ARM
reproduces to the digit on the same binary and the same pose list. Since the arm
matches, neither the pose set (the two undocumented extras t=2980/t=5518 are on
the AUTHORED camera — confirmed by the residual breakdown matching) nor the
metric nor the binary is at fault; the published "flat" row's exact flag-set is
not recorded anywhere in the repo and cannot be traced. **The comparative claim it
was used for survives unchanged: the shell arm's void (10) is still BELOW the
undisplaced arm's own rasterizer-crack count (18), so the metric has still
bottomed out against the engine.**

## S1d-8.1 VOID / BLACK over the 19 poses — void is MIP-INVARIANT

| arm | mips OFF | mips ON | Δ |
|---|--:|--:|--:|
| **the §S1d-5b.10 arm** | **10 / 73** | **10 / 52** | void **0**, black −21 |
| flat, `--texture_filter=1` | 18 / 139 | 18 / 120 | void **0**, black −19 |
| flat, bare `--deferred` | 18 / 146 | 18 / 132 | void **0**, black −14 |
| flat + POM (no shell) | 18 / 139 | — | — |

**Void does not move by a single pixel at any pose**, and the residual is the same
three poses with the same counts (7 @ t=5518, 2 @ t=5773, 1 @ t=5877). That is the
expected result stated as a mechanism, not an excuse: `void = popcount(z16==0)`
counts pixels **no fragment was ever rasterised into**, and mip selection changes
what a covered pixel SAMPLES, not what gets covered. (`--mip_fix` does move the
subdivision cut lines, so this was not free by construction — it is measured.)

**Black drops ~25 % on every arm** (73→52, 139→120, 146→132). Per §S1d-5b.10 the
black metric is not comparable across a texture-sampling change, and this is the
same effect one step further: coarser mips average a few pure-black stone texels
away. It is a metric artefact, not a coverage improvement.

**The flat control is mandatory here and it holds: 10 < 18 at both mip settings.**

## S1d-8.2 THE GRAZING SMEAR — the flip does NOT move it, and trilinear CANNOT

Sweep A rebuilt (t=5958, p5958a→p5958b, 16 frames, 0.040 world/frame lateral
dolly, `FDS_DUMP_TXTR=1`). The dumped `uf/vf` are in UV-repeat units; **the
published metric's texel scale is ×1024** (`greets_wall_h.png` / `greets_floor_h.png`
are both 1024²), which is what makes the ladder reproduce exactly.

| arm | slip p90 mips OFF | slip p90 mips ON | slip p90 mips ON + `--texture_filter=2` |
|---|--:|--:|--:|
| flat POM | 0.0944 | 0.0947 | 0.0944 |
| cap 2 | 0.2541 | 0.2546 | — |
| **cap 4 (the arm)** | **0.8910** | **0.8917** | **0.8912** |
| cap 8 | 2.5379 | 2.5390 | — |
| cap 16 | 6.2705 | 6.2717 | 6.2708 |

**THE SMEAR DID NOT MOVE. Not at any cap, not by 0.1 %.** Reach p90 is identical
to three decimals as well (7.19 / 26.85 / 47.28 / 88.89 / 170.64).

**And trilinear cannot move it, structurally — this is the finding, not the
number.** `slip` is a **marched-UV** metric: it is `uv − uvgeo` at the pixel, and
the march runs *upstream* of the albedo filter. `--texture_filter=2` changes how
the texel at the marched UV is *fetched*; it cannot change *which* UV the march
returns. So a slip ladder is blind to the filter by construction, and the campaign
should stop expecting a filter change to show up in it.

What the filter and the flip actually do at the smear pose, measured directly
(t=5958 p5958b, the arm, full-frame colour A/B):

| A/B | px changed | mean \|d\| on changed | px >12/255 | max |
|---|--:|--:|--:|--:|
| `--texture_filter=1` vs `=2` (mips ON) | 406 838 (19.6 %) | 1.02 | **182** | 107 |
| mips OFF vs mips ON (tf=1) | 1 339 474 (64.6 %) | 1.03 | **462** | 111 |
| height mip auto vs `--pom_height_mip=0`, t=5958 | 460 772 (22.2 %) | 1.01 | **73** | 83 |
| height mip auto vs `--pom_height_mip=0`, t=5743 | 399 824 (19.3 %) | 1.27 | **2 436** | 163 |

Both touch a fifth to two thirds of the frame **at ~1 LSB**. The height field the
march walks *is* coarser in places after the flip (`--pom_height_mip=0` is not a
no-op: 2 436 px >12/255 at t=5743) — but at the grazing poses the near wall is
mip 0 (79.8 % of screen AREA at level 0 at t=5958), which is why the marched UV,
and therefore the smear, is untouched.

**The grazing smear remains open, and the mip flip is now measured NOT to be a
lead on it.** The mechanism is unchanged: it is the cap-bounded `1/(V·N)` reach on
the lid march (§S1d-4.1, §S1d-5b.8).

## S1d-8.3 THE SILHOUETTE — byte-for-byte unchanged by the flip

`tools/greets_silhouette.py`, t=5877, 1920×1080, matched world amplitude 0.18 on
both displacement arms, flat passed first as the `off` datum:

| arm | med | off | std | p95 | rng | area | tv-net |
|---|--:|--:|--:|--:|--:|--:|--:|
| flat (OFF **and** ON) | 978.0 | 0.0 | 0.46 | 1.00 | 1 | 188 | 0 |
| tessellation @0.18 (OFF **and** ON) | 983.0 | +5.0 | 2.43 | 5.00 | 17 | 1045 | 42 |
| the shell arm (OFF **and** ON) | 950.0 | −28.0 | 0.35 | 1.00 | 1 | 85 | 0 |
| the shell arm + `--pom_shell_lid_planar` | 950.0 | −28.0 | 0.35 | 1.00 | 1 | 85 | 0 |

**Every column is identical at both mip settings** — the two tables are the same
table. Expected and now measured: the metric traces the first NEAR pixel per row
off the `z16` plane, and mips change texture sampling, not where the lid
geometry projects. **§S1d-6 stands entirely: the shell's silhouette is still
straighter than flat (0.35 vs 0.46) and still sits 28 px outside the authored
footprint.** The S1d-7 fix below does not touch it either.

## S1d-8.4 PERF at t=5743 — the flip is not neutral for the POM arms, and
## tessellation is now measurably CHEAPER than the shell arm

`--bench=scene@scene=greets,t=5743,iters=20`, interleaved, min-of-arm, **14 rounds
over two batches** (8 at load 10–21, 6 at load 5.8–6.3; a third 6-round batch at
load 16–43 is discarded for absolute values and agrees on every ordering). Both
statistics recorded: `bench` = min over rounds of the bench mean; `fmin` = min over
rounds of the profiler's `frame_ms min`.

| arm | bench OFF | bench ON | **fmin OFF** | **fmin ON** | fmin Δ flat OFF | fmin Δ flat ON |
|---|--:|--:|--:|--:|--:|--:|
| flat | 51.10 | 50.70 | **49.41** | **49.32** | — | — |
| + POM | 56.28 | 56.47 | **54.77** | **54.13** | +5.36 | +4.81 |
| + tessellation @0.18 | 57.32 | 56.27 | **55.43** | **54.77** | +6.02 | +5.45 |
| **the §S1d-5b.10 arm** | 58.60 | 57.85 | **56.50** | **55.81** | **+7.09** | **+6.49** |

**Noise floor on this harness: 0.09–0.25 ms** (flat OFF vs flat ON, two arms whose
true separation `SESSION_STATE` measures at ~0.1 ms). Nothing below ~0.3 ms is
quoted as resolved.

Two results, both above that floor and both reproduced in two independent batches:

1. **`--mips` ON is NOT perf-neutral for the parallax arms.** It buys
   **−0.6 ms (POM), −0.7 ms (tessellation), −0.7 ms (the shell arm)** and only
   −0.1 ms on flat. The flip's own measurement (`SESSION_STATE`: RNDR 39.855 vs
   39.965 at greets t=2993) was on a **non-POM** arm at a different pose; it is not
   contradicted, it just did not cover this. Mechanism (inferred, not measured):
   the parallax arms are the ones doing many dependent height/cone/albedo gathers,
   so they are the ones with a texture-cache win to collect.
2. **Tessellation is CHEAPER than the shell arm, at both mip settings.**
   `arm − tess` = **+1.07 ms** (fmin OFF), **+1.04 ms** (fmin ON), +1.28 / +1.58 on
   `bench` min. **§S1d-5b.6's +0.32 ms — the "they cost the same" result — does NOT
   reproduce.** The direction is the same and the gap is still ~1 ms on a ~56 ms
   frame, but the strategic sentence has to change: *at matched world amplitude
   the geometric carve is now the cheaper of the two, not the equal.* The mip flip
   is NOT what did it (the gap is the same either side of it, 1.07 vs 1.04).

`arm − flat` reproduces the published +7.44 exactly at mips OFF (+7.09 fmin,
+7.50 bench). `--pom_shell_lid_planar` is free: 56.46 vs 56.58 fmin min, 58.49 vs
57.91 bench min over 5 interleaved rounds — inside the floor, in both directions.

## S1d-8.5 THE QUAD DIAGONAL (S1d-7) — the non-planar lid quad is CONFIRMED, and `--pom_shell_lid_planar` removes the crease

**Reproduced post-flip** at the user's own repro, t=2993, cam
`-14.1865349,2.84484363,-53.351326,0.998402655,-0.0386195704,0.0412385091`,
1920×1080: the crease is present at **both** mip settings, plain POM has none.
Mips are irrelevant to it, as §S1d-7 predicted.

**THE CENSUS §S1d-7 ASKED FOR, built and run** (`--pom_shell_census` now prints
`[POM-SHELL-QUADPLANE]`: per pair of target faces that shared an edge AND one
AUTHORED plane — the quad's triangulation diagonal — the angle between the two
RE-DERIVED lid normals, and the plane gap evaluated at the partner triangle's
centroid):

| material | pairs | lid-normal angle mean | max | plane gap at partner centroid, max | <0.01° | <0.1° | <0.5° | <2° | <10° |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| **`rooms`** (the walls, where the crease is) | 133 | **0.659°** | **3.068°** | **0.0878 world** | 10 | 17 | 56 | 35 | 15 |
| `floor` | 24 | **0.0000°** | 0.0000° | 0.00000 | 24 | 0 | 0 | 0 | 0 |

**The hypothesis is confirmed as a real, large population.** 123 of 133 wall pairs
carry a non-zero angle; the worst plane gap is **0.0878 world against a lid offset
of 0.0900** — one triangle's plane is nearly a full amplitude away from its own
quad-partner's at that partner's centroid. And `floor` is the perfect internal
control: it reports `0 corner verts (ShellH min 1.000)` — no smoothed-normal
divergence, therefore exactly 0.0000° on all 24 pairs. **The effect appears
exactly where corner verts exist and nowhere else.**

**THE CAUSAL A/B — `--pom_shell_lid_planar` (new, default 0, byte-null).** It gives
every lid face of ONE AUTHORED PLANE a single lid plane: the area-weighted mean
normal over the shell's own union-find patch (which already *is* "the faces sharing
one authored plane"), with the plane constant re-fitted to that patch's
area-weighted centroid. **No vertex moves**; only the per-face plane the march
reads changes.

| arm at t=2993 | diagonal crease |
|---|---|
| plain POM, no shell | absent |
| `--pom_shell` (minimal), mips OFF **and** ON | **present**, sharp, on the diagonal |
| the full §S1d-5b.10 arm, mips ON | **present** |
| `--pom_shell --pom_shell_entry_flat` (§S1d-7) | present, attenuated |
| **`--pom_shell --pom_shell_lid_planar`** | **GONE** |
| **the full arm + `--pom_shell_lid_planar`** | **GONE** |

`docs/img/s1d_7/t2993_quad_diagonal_lid_planar.png` is the three-way crop.
The flag moves 57 394 px at this pose (25 903 >12/255).

**And it costs nothing measured:**

| gate | the arm | the arm + `--pom_shell_lid_planar` |
|---|---|---|
| 19 poses, mips ON | 10 / 52 | **10 / 52** |
| 19 poses, mips OFF | 10 / 73 | **10 / 73** |
| silhouette t=5877 | std 0.35, off −28 | **std 0.35, off −28** (unchanged) |
| t=5743 perf | 56.46 fmin / 58.49 bench | **56.58 / 57.91** (inside the noise floor) |

**What it does NOT do: it does not fix the silhouette.** §S1d-7 hoped one fix might
serve both defects because §S1d-6.4 also wants "the march should read the authored
plane". Measured: **it does not.** The silhouette is byte-identical with the flag
on. The overhang is created by *where the lid vertices sit* (`worldAmp/2` along N
past the patch border), and unifying the *plane* moves no vertex. **Two defects,
two fixes.**

**Recommendation: fly `--pom_shell_lid_planar` with the arm.** It removes a
user-reported, plainly visible defect at zero measured cost on every other metric.
It is left default OFF pending the user's eyes on a moving camera.

## S1d-8.6 WHAT THE NEW BASELINE MAKES CHEAPEST TO ATTACK NEXT

Ranked by (defect visible to the user) ÷ (measurement cost now known):

1. **The silhouette (§S1d-6.4)** — the one defect the user reported by eye that is
   still open, with a validated instrument, a known root cause, four measured dead
   ends and two untried candidates already written down. Mips are now measured
   irrelevant to it, so the search space did not grow. **Candidate 2 (gate on the
   base clip's OVERSHOOT DISTANCE, not its boolean) is the cheap one** — the
   quantity is already computed inside the base clip.
2. **The grazing smear** — now measured to be mip-independent AND filter-independent
   *in the marched-UV metric*. Before spending more on it, the campaign needs a
   metric that is not blind to filtering, because the user's complaint ("swimming
   textures") is about what the *pixel* does, and every instrument in this document
   scores the *march*. That is a real gap, and it is why cap 4 vs cap 16 "costs
   slip" numbers may be overstating what the eye sees.
3. **The `rooms` vs `floor` asymmetry the census exposed.** `floor` has zero corner
   verts and zero non-planarity; `rooms` has 438 of 588 moved verts corner-corrected.
   Whatever the smoothed-normal offset is buying on the walls, it is what
   manufactured the crease — and `--pom_shell_lid_planar` throws its *plane* away
   while keeping its *positions*. Whether the positions should be projected onto
   the authored plane too (the other half of §S1d-7's "make the lid planar") is
   untried and is now a one-flag experiment.

## S1d-8.7 REPRODUCTION

```sh
# isolated binary (so four concurrent agents cannot contaminate a figure)
git worktree add --detach /path/rev-s1d8 <sha>
cmake -S /path/rev-s1d8 -B /path/rev-s1d8/build -G Ninja \
      -DMODPLAYER_DIR=$PWD/Modplayer/modplayer && cmake --build /path/rev-s1d8/build
# ...then run it with cwd = the MAIN tree's Runtime/, so the user's uncommitted
# authoring assets are the ones rendered (the greets pin depends on them).

# 19-pose battery: poses = 16 lines of docs/greets_review_poses.txt
#   + t=2980 and t=5518 on the AUTHORED camera (FDS_GREETS_CAM UNSET)
#   + t=5877 on the S1d-6.1 silhouette cam.
#   FDS_SNAPSHOT_ZDUMP=1; void = popcount(z16==0), black = popcount(max rgb==0)

# sweep A (slip/reach): 16 frames, lerp cam pos p5958a -> p5958b, dir fixed,
#   FDS_DUMP_TXTR=1; offset = (uv - uvgeo) * 1024  [TEXELS - the x1024 is what
#   makes the published ladder reproduce]; cells = floor(uvgeo*1024), mean
#   offset per cell; slip = |offset(f) - offset(f-1)| pooled over 15 pairs.

# the quad-plane census (init-time print, changes no pixel)
./DEMO --snapshot=greets@t=2993 --deferred --pom_shell --pom_shell_world_amp \
       --pom_shell_world_amp_set=0.18 --parallax_pom_cone --parallax_pom=32 \
       --pom_cone_exact=1 --pom_cone_min_step=1 --texture_filter=1 \
       --pom_shell_census 2>&1 | grep QUADPLANE
```

**Gates.** All measurement above ran on `97b13fd`, where the greets pin is
**`adfba8ba`** (mips ON) and **`f1297141`** (`--no-mips --no-mip_fix`), plus
`render_gate` 3/3 (`4ac809e5` / `b41894f9` / `166fa25a`) and fountain
**`8db68ccb`** — all byte-exact.

**Byte-null proof for `--pom_shell_lid_planar`, taken the only way that is valid
on a tree four agents are committing to: against its OWN PARENT, same binary
recipe.** By the time this landed the greets pin had already moved to
**`6780642b`** under another agent's `--hdr_metal_kill` default 0→2. Built at the
parent `06b6291` it gives `6780642b`; built at this commit it gives `6780642b`.
Identical, so this commit moves nothing.

**NOT A GATE, AND NOT MINE — the city pin does not reproduce.**
`FDS_CITY_ENV_PIXEL=1 --snapshot=city@t=1961 --deferred` gives `5476be8c…`
against the recorded `e1221676…`, **stably (2/2 identical runs, so it is a real
drift and not the old nondeterminism)**, and the `--no-mips --no-mip_fix` control
also fails (`b88ecb7b…` against `37e62845…`). Both halves of the pair moving rules
out the mip flip; `--pom_shell_lid_planar` cannot be involved (default off, never
runs outside `PomShell_Build`); and the `--hdr_metal_kill` re-pin explicitly
records that city did **not** move for it. So something else between the
2026-08-08 city re-pin and `97b13fd` moved it — the env-reflection work is the
obvious suspect, since this recipe is the env-pixel gate. Flagged, not chased.
