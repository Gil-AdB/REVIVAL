# S1d — Closed shell + cross-patch march continuation

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
| greets **recess** arm, 13 review poses, depth md5 | **byte-identical** to the pre-change binary |
| greets **lid** arm, 13 review poses, depth md5 | **byte-identical** to the pre-change binary |
| recess + `weld=2` + `side_entry=1` + `lid_edge=1`, 13 poses | **byte-identical** to plain recess (all three inert there) |
| recess `side_faces=1` vs `=3`, 13 poses | **byte-identical** (mode 3 is a lid-only correction) |
| wasm | `cmake --build build-wasm` links clean (82/82) |
| bad flags | **0** across 578 snapshot run logs + the shadow-dump runs |

Greets COLOUR remains unusable as a byte gate: the flags-off pair differs at
13/13 poses, and the difference splits into two populations — max delta 1 (pure
rounding) at 6 poses and max delta 187–241 at 7, i.e. the known lightmap-bake
flip. Depth was stable and is what every byte claim above rests on.

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
